/* createFontEngine is prepended by the single-file packager. */
'use strict';
let engine;

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// Stored ZIP entries keep generation independent of browser compression support.
function makeZip(files) {
  const encoder = new TextEncoder();
  const locals = [], central = [];
  let offset = 0, centralSize = 0;
  for (const file of files) {
    const name = encoder.encode(file.name), data = file.data;
    if (name.length > 65535 || data.length > 0xffffffff) throw new Error('Output exceeds ZIP format limits.');
    const crc = crc32(data);
    const local = new Uint8Array(30 + name.length), lv = new DataView(local.buffer);
    lv.setUint32(0, 0x04034b50, true); lv.setUint16(4, 20, true);
    lv.setUint16(6, 0x0800, true); lv.setUint16(12, 0x21, true);
    lv.setUint32(14, crc, true); lv.setUint32(18, data.length, true); lv.setUint32(22, data.length, true);
    lv.setUint16(26, name.length, true); local.set(name, 30);
    const entry = new Uint8Array(46 + name.length), cv = new DataView(entry.buffer);
    cv.setUint32(0, 0x02014b50, true); cv.setUint16(4, 20, true); cv.setUint16(6, 20, true);
    cv.setUint16(8, 0x0800, true); cv.setUint16(14, 0x21, true);
    cv.setUint32(16, crc, true); cv.setUint32(20, data.length, true); cv.setUint32(24, data.length, true);
    cv.setUint16(28, name.length, true); cv.setUint32(42, offset, true); entry.set(name, 46);
    locals.push(local, data); central.push(entry);
    offset += local.length + data.length; centralSize += entry.length;
  }
  if (offset + centralSize > 0xffffffff || files.length > 65535) throw new Error('Font pack exceeds ZIP format limits.');
  const end = new Uint8Array(22), ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, files.length, true); ev.setUint16(10, files.length, true);
  ev.setUint32(12, centralSize, true); ev.setUint32(16, offset, true);
  return new Blob([...locals, ...central, end], {type: 'application/zip'});
}

self.onmessage = async ({data}) => {
  try {
    if (data.type !== 'generate') return;
    engine = await createFontEngine({
      wasmBinary: data.wasm,
      print: text => self.postMessage({type: 'log', text}),
      printErr: text => self.postMessage({type: 'log', text}),
      // Emscripten resolves this label even when wasmBinary supplies all bytes.
      locateFile: () => 'embedded-engine.wasm'
    });
    engine.FS.mkdirTree('/input');
    for (const file of data.inputs) engine.FS.writeFile(file.path, new Uint8Array(file.bytes));
    const files = [], warnings = new Set();
    const request = data.request;
    for (let index = 0; index < request.sizes.length; ++index) {
      const size = request.sizes[index];
      self.postMessage({type: 'progress', done: index, total: request.sizes.length, text: `Converting ${size} pt…`});
      const pointer = engine.ccall('convert', 'number', ['string'], [JSON.stringify({...request, sizes: [size]})]);
      if (!pointer) throw new Error('The converter could not allocate its result. Try fewer sizes or smaller coverage.');
      let result;
      try { result = JSON.parse(engine.UTF8ToString(pointer)); }
      finally { engine.ccall('free_result', null, ['number'], [pointer]); }
      if (result.error) throw new Error(result.error);
      for (const warning of result.warnings || []) warnings.add(warning);
      for (const file of result.files) {
        if (!file.path.startsWith('/output/')) throw new Error('Unexpected output path from converter.');
        files.push({name: file.path.slice('/output/'.length), data: engine.FS.readFile(file.path), kind: file.kind});
      }
    }
    for (let index = 0; index < data.licenses.length; ++index) {
      const file = data.licenses[index];
      const safeName = file.name.replace(/[^A-Za-z0-9._-]/g, '_');
      files.push({name: `fonts/${request.family}/licenses/${index + 1}-${safeName}`, data: new Uint8Array(file.bytes), kind: 'license'});
    }
    self.postMessage({type: 'progress', done: request.sizes.length, total: request.sizes.length, text: 'Packaging font pack…'});
    const zip = makeZip(files);
    const previews = files.filter(file => /\.(cpfont|cpshape)$/.test(file.name));
    self.postMessage({type: 'complete', zip, warnings: [...warnings],
      files: files.map(file => ({name: file.name, size: file.data.length})), previews},
      previews.map(file => file.data.buffer));
  } catch (error) {
    self.postMessage({type: 'error', message: error instanceof Error ? error.message : String(error)});
  }
};
