'use strict';
(() => {
  const $ = id => document.getElementById(id);
  const styleNames = ['Regular', 'Bold', 'Italic', 'Bold italic'];
  const han = [[0x2e80,0x2eff],[0x2f00,0x2fdf],[0x3000,0x303f],[0x3100,0x312f],[0x31a0,0x31bf],[0x3400,0x4dbf],[0x4e00,0x9fff],[0xf900,0xfaff],[0xff00,0xffef]];
  const presets = {
    'Reading (Fiction)': [[0x20,0x24f],[0x2b0,0x3ff],[0x400,0x4ff],[0x1e00,0x1eff],[0x2000,0x206f],[0x2070,0x209f],[0x20a0,0x20cf],[0x2150,0x218f],[0x2190,0x21ff],[0x2200,0x22ff],[0x2500,0x257f],[0x25a0,0x27bf],[0x2900,0x29ff],[0x2e00,0x2e7f],[0x3000,0x303f],[0xfb00,0xfb06]],
    'Default (CrossPoint)': [[0,0x17f],[0x1a0,0x1a1],[0x1af,0x1b0],[0x1c4,0x21f],[0x300,0x36f],[0x400,0x4ff],[0x1ea0,0x1ef9],[0x2000,0x206f],[0x20a0,0x20cf],[0x2070,0x209f],[0x2190,0x21ff],[0x2200,0x22ff],[0xfb00,0xfb06]],
    'Latin Extended': [[0x20,0x24f],[0x2b0,0x2ff],[0x1e00,0x1eff],[0x2000,0x206f],[0xfb00,0xfb06]],
    'Greek': [[0x370,0x3ff],[0x1f00,0x1fff]],
    'Cyrillic': [[0x400,0x52f]],
    'Vietnamese': [[0x1a0,0x1b0],[0x1ea0,0x1ef9]],
    'Hebrew': [[0x590,0x5ff],[0xfb1d,0xfb4f]],
    'Arabic (Farsi, Urdu)': [[0x600,0x6ff],[0x750,0x77f],[0x8a0,0x8ff],[0xfb50,0xfdf9],[0xfe70,0xfeff]],
    'Armenian': [[0x530,0x58f]],
    'Georgian': [[0x10a0,0x10ff],[0x2d00,0x2d2f]],
    'Ethiopic': [[0x1200,0x137f],[0x1380,0x139f],[0x2d80,0x2ddf]],
    'Cherokee': [[0x13a0,0x13ff],[0xab70,0xabbf]],
    'Tifinagh': [[0x2d30,0x2d7f]],
    'Thai': [[0xe00,0xe7f]],
    'Hangul (Korean)': [[0x1100,0x11ff],[0x3130,0x318f],[0xac00,0xd7af]],
    'Chinese (Simplified)': han,
    'Chinese (Traditional)': han,
    'Japanese': [[0x3000,0x30ff],[0x31f0,0x31ff],[0x3400,0x4dbf],[0x4e00,0x9fff],[0xf900,0xfaff],[0xff00,0xffef]],
    'Symbols & Arrows': [[0x2070,0x209f],[0x20a0,0x20cf],[0x2150,0x218f],[0x2190,0x21ff],[0x2200,0x22ff],[0x2500,0x257f],[0x25a0,0x27bf]],
    'IPA characters': [[0x250,0x2ff]]
  };
  const defaultCoverage = new Set(['Reading (Fiction)', 'Default (CrossPoint)', 'Arabic (Farsi, Urdu)', 'Thai', 'Symbols & Arrows', 'IPA characters']);
  let worker = null, workerUrl = null, downloadUrl = null, engineRequest = null, previews = [], generation = 0;
  const embeddedBytes = id => Uint8Array.from(atob($(id).textContent.trim()), ch => ch.charCodeAt(0));
  const embeddedText = id => new TextDecoder().decode(embeddedBytes(id));
  if ($('license-source').textContent.trim()) $('notices').textContent = embeddedText('license-source');
  if (location.protocol === 'file:') {
    document.querySelector('.site-nav a[lang=th]').hidden = true;
    document.querySelector('header .brand').removeAttribute('href');
  }

  function checkbox(parent, value, checked, name) {
    const label = document.createElement('label'); label.className = 'check';
    const input = document.createElement('input'); input.type = 'checkbox'; input.value = value;
    input.checked = checked; input.name = name;
    label.append(input, document.createTextNode(String(value))); $(parent).append(label);
  }
  for (const size of [8,10,12,14,16,18,20,22,24,26]) checkbox('sizes', size, size <= 18, 'size');
  Object.keys(presets).forEach(preset => checkbox('presets', preset, defaultCoverage.has(preset), 'preset'));
  const thaiCoverage = document.querySelector('input[name=preset][value=Thai]');
  thaiCoverage.addEventListener('change', () => { if (!thaiCoverage.checked) $('thai').checked = false; });
  $('thai').addEventListener('change', () => { if ($('thai').checked) thaiCoverage.checked = true; });
  for (let id = 1; id < 4; ++id) {
    const row = document.createElement('div'); row.className = 'style-row';
    row.innerHTML = `<label for="style-${id}">${styleNames[id]} font</label><input id="style-${id}" type="file" accept=".ttf,.otf"><label for="fallback-${id}" style="margin-top:12px">${styleNames[id]} fallback fonts</label><input id="fallback-${id}" type="file" accept=".ttf,.otf" multiple><p class="hint" id="fallback-order-${id}">No fallback fonts selected.</p>`;
    $('extra-styles').append(row);
  }
  for (let id = 0; id < 4; ++id) {
    $(`fallback-${id}`).addEventListener('change', () => {
      $(`fallback-order-${id}`).textContent = Array.from($(`fallback-${id}`).files, (file, index) => `${index + 1}. ${file.name}`).join(' → ') || 'No fallback fonts selected.';
    });
  }
  $('regular').addEventListener('change', () => {
    if ($('family').value === 'MyFont' && $('regular').files[0]) {
      const stem = $('regular').files[0].name.replace(/\.(ttf|otf)$/i, '').replace(/[^A-Za-z0-9_-]/g, '-').replace(/^[^A-Za-z0-9]+/, '');
      $('family').value = stem.slice(0, 64) || 'MyFont';
    }
  });

  function status(text, state = '') { $('status').textContent = text; $('status').dataset.state = state; }
  function busy(value) {
    $('settings').disabled = value; $('generate').disabled = value; $('cancel').hidden = !value;
    $('progress').hidden = !value;
  }
  function stopWorker() {
    if (engineRequest) engineRequest.abort(); engineRequest = null;
    if (worker) worker.terminate(); worker = null;
    if (workerUrl) URL.revokeObjectURL(workerUrl); workerUrl = null;
  }
  function clearResults() {
    if (downloadUrl) URL.revokeObjectURL(downloadUrl); downloadUrl = null;
    $('download').hidden = true; $('download').removeAttribute('href'); $('install').hidden = true;
    $('files').replaceChildren(); $('warnings').replaceChildren(); $('log').textContent = '';
    $('warning-details').hidden = true; $('warning-details').open = false;
    $('warning-summary').textContent = 'Warnings';
    $('log-details').hidden = true; $('preview').hidden = true; $('empty-proof').hidden = false;
    $('proof-controls').hidden = true; previews = [];
  }
  function fail(message) { stopWorker(); busy(false); status(message, 'error'); }
  $('cancel').addEventListener('click', () => {
    ++generation; stopWorker(); busy(false); status('Cancelled. No font pack was published.');
  });

  function intervals() {
    const result = [[0x20,0x7e],[0x2000,0x206f],[0xfffd,0xfffd]];
    for (const input of document.querySelectorAll('input[name=preset]:checked')) result.push(...presets[input.value]);
    const custom = $('ranges').value.trim();
    if (custom) for (const part of custom.split(',')) {
      const match = part.trim().match(/^(?:U\+)?([0-9a-f]{1,6})(?:\s*-\s*(?:U\+)?([0-9a-f]{1,6}))?$/i);
      if (!match) throw new Error(`Invalid Unicode range: ${part.trim()}. Use a range such as 0300-036F.`);
      const start = parseInt(match[1], 16), end = parseInt(match[2] || match[1], 16);
      if (start > end || end > 0x10ffff || (start <= 0xdfff && end >= 0xd800)) throw new Error(`Invalid Unicode scalar range: ${part.trim()}.`);
      result.push([start, end]);
    }
    if ($('thai').checked) result.push(...presets.Thai);
    return result;
  }

  $('generator').addEventListener('submit', async event => {
    event.preventDefault();
    const current = ++generation;
    stopWorker(); clearResults();
    try {
      if (!globalThis.WebAssembly || !globalThis.Worker) throw new Error('This browser needs WebAssembly and Web Worker support. Use a current desktop browser.');
      const sizes = Array.from(document.querySelectorAll('input[name=size]:checked'), input => Number(input.value));
      if (!sizes.length) throw new Error('Select at least one point size.');
      const request = {family: $('family').value.trim(), sizes, intervals: intervals(), thai: $('thai').checked, autohint: $('autohint').checked, styles: []};
      const inputs = [], licenses = [];
      busy(true); status('Reading font files…'); $('progress').value = 0;
      async function addInput(file) {
        const path = `/input/${inputs.length}-${file.name.replace(/[^A-Za-z0-9._-]/g, '_')}`;
        if (!/\.(ttf|otf)$/i.test(file.name)) throw new Error('Choose a TTF or OTF font file. Collections and web fonts are not supported.');
        inputs.push({path, bytes: await file.arrayBuffer()}); return path;
      }
      for (let id = 0; id < 4; ++id) {
        const source = $(id === 0 ? 'regular' : `style-${id}`).files[0];
        const fallbackFiles = Array.from($(`fallback-${id}`).files);
        if (!source) {
          if (id === 0) throw new Error('Choose a regular font.');
          if (fallbackFiles.length) throw new Error(`Choose a ${styleNames[id].toLowerCase()} primary font before adding its fallbacks.`);
          continue;
        }
        const path = await addInput(source), fallbacks = [];
        for (const file of fallbackFiles) fallbacks.push(await addInput(file));
        request.styles.push({id, path, fallbacks});
      }
      for (const file of $('licenses').files) licenses.push({name: file.name, bytes: await file.arrayBuffer()});
      if (current !== generation) return;
      const wasmUrl = $('wasm-source').dataset.url;
      let wasm;
      if (wasmUrl) {
        status('Loading font engine…');
        engineRequest = new AbortController();
        const response = await fetch(wasmUrl, {signal: engineRequest.signal, credentials: 'omit'});
        if (!response.ok) throw new Error(`Could not load the font engine (HTTP ${response.status}). Ensure the WASM file is deployed beside this page.`);
        wasm = new Uint8Array(await response.arrayBuffer());
        if (current !== generation) return;
        engineRequest = null;
      } else {
        wasm = embeddedBytes('wasm-source');
      }
      status('Starting font engine…');
      workerUrl = URL.createObjectURL(new Blob([embeddedText('engine-source'), '\n', embeddedText('worker-source')], {type: 'text/javascript'}));
      worker = new Worker(workerUrl);
      worker.onerror = event => { if (current === generation) fail(event.message || 'The font worker stopped unexpectedly. Try a smaller character set.'); };
      worker.onmessage = ({data}) => {
        if (current !== generation) return;
        if (data.type === 'log') {
          $('log-details').hidden = false;
          $('log').textContent = ($('log').textContent + data.text + '\n').slice(-16000);
        } else if (data.type === 'progress') {
          status(data.text); $('progress').max = data.total; $('progress').value = data.done;
        } else if (data.type === 'error') fail(data.message);
        else if (data.type === 'complete') {
          stopWorker(); busy(false);
          status(`Ready: ${request.sizes.length} size${request.sizes.length === 1 ? '' : 's'}, ${request.styles.length} style${request.styles.length === 1 ? '' : 's'}.`, 'success');
          for (const warning of data.warnings) { const li = document.createElement('li'); li.textContent = warning; $('warnings').append(li); }
          $('warning-summary').textContent = `Warnings (${data.warnings.length})`;
          $('warning-details').hidden = data.warnings.length === 0;
          for (const file of data.files) { const li = document.createElement('li'); li.textContent = `${file.name} · ${(file.size / 1024).toFixed(1)} KiB`; $('files').append(li); }
          downloadUrl = URL.createObjectURL(data.zip); $('download').href = downloadUrl;
          $('download').download = `${request.family}-fonts.zip`; $('download').hidden = false; $('install').hidden = false;
          previews = data.previews; $('proof-file').replaceChildren();
          for (const file of previews.filter(file => file.name.endsWith('.cpfont'))) {
            const option = document.createElement('option'); option.value = file.name; option.textContent = file.name.split('/').pop(); $('proof-file').append(option);
          }
          $('proof-controls').hidden = false; updatePreviewStyles();
        }
      };
      worker.postMessage({type: 'generate', request, inputs, licenses, wasm}, [wasm.buffer, ...inputs.map(file => file.bytes), ...licenses.map(file => file.bytes)]);
    } catch (error) { if (current === generation) fail(error.message || String(error)); }
  });

  // Decode the actual output format; never use Canvas text as a glyph substitute.
  function parseFont(bytes, styleId) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const u32 = pos => view.getUint32(pos, true), u16 = pos => view.getUint16(pos, true), i16 = pos => view.getInt16(pos, true);
    let toc = 32;
    while (toc < 32 + bytes[12] * 32 && bytes[toc] !== styleId) toc += 32;
    if (toc >= 32 + bytes[12] * 32) throw new Error('Preview style is absent.');
    const intervalCount = u32(toc + 4), glyphCount = u32(toc + 8), offset = u32(toc + 24);
    const glyphStart = offset + intervalCount * 12;
    const bitmapStart = glyphStart + glyphCount * 16 + 3 * (u16(toc + 17) + u16(toc + 19)) + bytes[toc + 21] * bytes[toc + 22] + bytes[toc + 23] * 8;
    return {ascender: i16(toc + 13), descender: i16(toc + 15), glyph(cp) {
      for (let index = 0; index < intervalCount; ++index) {
        const interval = offset + index * 12;
        if (cp < u32(interval) || cp > u32(interval + 4)) continue;
        const at = glyphStart + (u32(interval + 8) + cp - u32(interval)) * 16;
        return {width: bytes[at], height: bytes[at + 1], advance: u16(at + 2), left: i16(at + 4), top: i16(at + 6), pixels: bytes.subarray(bitmapStart + u32(at + 12), bitmapStart + u32(at + 12) + u16(at + 8))};
      }
      return null;
    }};
  }
  function shapeRecipe(bytes, styleId, cps) {
    if (!bytes || cps[0] < 0xe01 || cps[0] > 0xe2e) return null;
    const firstSigns = [null,0xe31,0xe34,0xe35,0xe36,0xe37,0xe38,0xe39,0xe3a,0xe47,0xe4d];
    const terminals = [null,0xe48,0xe49,0xe4a,0xe4b,0xe4c,0xe4e];
    let suffix;
    if (cps.at(-1) === 0xe33) {
      const tone = cps.length === 3 ? cps[1] : null;
      const index = [null,0xe48,0xe49,0xe4a,0xe4b].indexOf(tone);
      if (index < 0) return null;
      suffix = 77 + index;
    } else {
      let first = null, terminal = null;
      if (cps.length > 1 && firstSigns.includes(cps[1])) { first = cps[1]; terminal = cps[2] ?? null; }
      else terminal = cps[1] ?? null;
      if (!terminals.includes(terminal)) return null;
      suffix = firstSigns.indexOf(first) * 7 + terminals.indexOf(terminal);
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), u32 = pos => view.getUint32(pos, true), u16 = pos => view.getUint16(pos, true);
    let style;
    for (let index = 0; index < u32(28); ++index) if (bytes[32 + index * 12] === styleId) style = u32(36 + index * 12);
    if (style === undefined) return null;
    const key = (cps[0] - 0xe01) * 82 + suffix;
    const dense = style + u32(style + 4) + key * 4;
    const baseId = u16(dense), suffixId = u16(dense + 2);
    if (baseId === 0xffff || suffixId === 0xffff) return null;
    const record = at => ({cp: u16(at), advance: u16(at + 2), x: view.getInt16(at + 4, true), y: view.getInt16(at + 6, true)});
    const records = [record(style + u32(style + 8) + baseId * 8)];
    const suffixAt = style + u32(style + u32(style + 12) + suffixId * 4);
    for (let index = 0; index < bytes[suffixAt]; ++index) records.push(record(suffixAt + 1 + index * 8));
    return records;
  }
  function updatePreviewStyles() {
    const file = previews.find(file => file.name === $('proof-file').value);
    $('proof-style').replaceChildren();
    for (let index = 0; index < file.data[12]; ++index) {
      const id = file.data[32 + index * 32], option = document.createElement('option');
      option.value = id; option.textContent = styleNames[id]; $('proof-style').append(option);
    }
    renderPreview();
  }
  function renderPreview() {
    try {
      const file = previews.find(file => file.name === $('proof-file').value), styleId = Number($('proof-style').value);
      const companion = previews.find(item => item.name === file.name.replace(/\.cpfont$/, '.cpshape'));
      const font = parseFont(file.data, styleId), samples = ['A','g','f','é','กิ','กี้','กุ','กู้','กำ','ก้ำ','ญุ','ปี่'];
      const canvas = $('preview'), columns = 4, tileW = 90, tileH = Math.max(88, font.ascender - font.descender + 32);
      canvas.width = columns * tileW; canvas.height = Math.ceil(samples.length / columns) * tileH;
      const ctx = canvas.getContext('2d'); ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
      samples.forEach((text, index) => {
        const x = (index % columns) * tileW, y = Math.floor(index / columns) * tileH;
        const cps = Array.from(text, ch => ch.codePointAt(0));
        const recipe = shapeRecipe(companion?.data, styleId, cps) || cps.map(cp => ({cp, advance: font.glyph(cp)?.advance || 0, x: 0, y: 0}));
        const width = recipe.reduce((sum, record) => sum + record.advance, 0) / 16;
        let pen = 0;
        ctx.strokeStyle = '#e9d9e2'; ctx.strokeRect(x + .5, y + .5, tileW - 1, tileH - 1);
        const baseline = y + 10 + font.ascender;
        ctx.fillStyle = '#e9d9e2'; ctx.fillRect(x + 8, baseline, tileW - 16, 1);
        ctx.save(); ctx.beginPath(); ctx.rect(x + 1, y + 1, tileW - 2, tileH - 22); ctx.clip();
        for (const record of recipe) {
          const glyph = font.glyph(record.cp);
          if (glyph) {
            const gx = x + Math.floor((tileW - width) / 2) + Math.floor((pen + record.x + 8) / 16) + glyph.left;
            const gy = baseline - glyph.top - Math.floor((record.y + 8) / 16);
            for (let py = 0; py < glyph.height; ++py) for (let px = 0; px < glyph.width; ++px) {
              const pixel = py * glyph.width + px, coverage = (glyph.pixels[pixel >> 2] >> (6 - (pixel % 4) * 2)) & 3;
              if (coverage) { const gray = 255 - coverage * 85; ctx.fillStyle = `rgb(${gray},${gray},${gray})`; ctx.fillRect(gx + px, gy + py, 1, 1); }
            }
          }
          pen += record.advance;
        }
        ctx.restore(); ctx.fillStyle = '#786b75'; ctx.font = '10px monospace'; ctx.textAlign = 'center';
        ctx.fillText(cps.map(cp => cp.toString(16).toUpperCase()).join(' '), x + tileW / 2, y + tileH - 8);
      });
      canvas.hidden = false; $('empty-proof').hidden = true;
    } catch (error) {
      $('preview').hidden = true; $('empty-proof').hidden = false;
      $('empty-proof').textContent = `Preview unavailable: ${error.message}. The generated files remain downloadable.`;
    }
  }
  $('proof-file').addEventListener('change', updatePreviewStyles);
  $('proof-style').addEventListener('change', renderPreview);
  addEventListener('beforeunload', () => { stopWorker(); if (downloadUrl) URL.revokeObjectURL(downloadUrl); });
})();
