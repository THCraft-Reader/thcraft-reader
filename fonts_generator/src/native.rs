use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::ptr::NonNull;

#[repr(C)]
#[derive(Default, Copy, Clone)]
pub struct Metrics {
    pub ascender: i32,
    pub descender: i32,
    pub line_height: i32,
    pub x_ppem: u32,
    pub y_ppem: u32,
}

#[repr(C)]
pub struct Bitmap {
    pub width: u32,
    pub height: u32,
    pub left: i32,
    pub top: i32,
    pub advance_fp: u32,
    pub pitch: i32,
    pub pixels: *const u8,
}

#[repr(C)]
#[derive(Default, Copy, Clone, Debug, PartialEq, Eq)]
pub struct ShapedGlyph {
    pub glyph_id: u32,
    pub advance_fp: i32,
    pub x_fp: i32,
    pub y_fp: i32,
    pub y_advance_fp: i32,
}

extern "C" {
    fn fg_open(path: *const c_char, point_size: c_int, autohint: c_int) -> *mut c_void;
    fn fg_close(face: *mut c_void);
    fn fg_glyph_index(face: *mut c_void, cp: u32) -> u32;
    fn fg_metrics(face: *mut c_void, metrics: *mut Metrics) -> c_int;
    fn fg_raster(face: *mut c_void, gid: u32, bitmap: *mut Bitmap) -> c_int;
    fn fg_shape(
        face: *mut c_void,
        cps: *const u32,
        count: u32,
        output: *mut ShapedGlyph,
        capacity: u32,
    ) -> c_int;
    fn fg_error() -> *const c_char;
    fn fg_freetype_version() -> *const c_char;
    fn fg_harfbuzz_version() -> *const c_char;
}

fn string(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return "Native engine returned no diagnostic".into();
    }
    unsafe { CStr::from_ptr(pointer).to_string_lossy().into_owned() }
}

fn error() -> String {
    unsafe { string(fg_error()) }
}

pub fn versions() -> (String, String) {
    unsafe { (string(fg_freetype_version()), string(fg_harfbuzz_version())) }
}

pub struct Face(NonNull<c_void>);

impl Face {
    pub fn open(path: &str, size: u8, autohint: bool) -> Result<Self, String> {
        let path_c = CString::new(path).map_err(|_| "Font path contains a NUL byte")?;
        let pointer = unsafe { fg_open(path_c.as_ptr(), i32::from(size), i32::from(autohint)) };
        NonNull::new(pointer)
            .map(Self)
            .ok_or_else(|| format!("Cannot open {path}: {}", error()))
    }

    pub fn glyph_index(&self, cp: u32) -> u32 {
        unsafe { fg_glyph_index(self.0.as_ptr(), cp) }
    }

    pub fn metrics(&self) -> Result<Metrics, String> {
        let mut metrics = Metrics::default();
        if unsafe { fg_metrics(self.0.as_ptr(), &mut metrics) } != 1 {
            return Err(error());
        }
        Ok(metrics)
    }

    // The closure cannot retain FreeType's bitmap across a subsequent glyph load.
    pub fn raster<T>(
        &mut self,
        gid: u32,
        consume: impl FnOnce(&Bitmap) -> Result<T, String>,
    ) -> Result<T, String> {
        let mut bitmap = Bitmap {
            width: 0,
            height: 0,
            left: 0,
            top: 0,
            advance_fp: 0,
            pitch: 0,
            pixels: std::ptr::null(),
        };
        if unsafe { fg_raster(self.0.as_ptr(), gid, &mut bitmap) } != 1 {
            return Err(error());
        }
        consume(&bitmap)
    }

    pub fn shape(&mut self, cps: &[u32]) -> Result<Vec<ShapedGlyph>, String> {
        let mut glyphs = [ShapedGlyph::default(); 6];
        let count = unsafe {
            fg_shape(
                self.0.as_ptr(),
                cps.as_ptr(),
                cps.len() as u32,
                glyphs.as_mut_ptr(),
                glyphs.len() as u32,
            )
        };
        if count < 0 {
            return Err(error());
        }
        if !(1..=6).contains(&count) {
            return Err(format!(
                "Thai context produced {count} glyphs; CPSHAPE allows 1–6"
            ));
        }
        Ok(glyphs[..count as usize].to_vec())
    }
}

impl Drop for Face {
    fn drop(&mut self) {
        unsafe { fg_close(self.0.as_ptr()) }
    }
}
