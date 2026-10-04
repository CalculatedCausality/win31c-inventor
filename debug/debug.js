/* PCjs 2.25 stable component bindings/exports; no minified internal methods. */
'use strict';
(() => {
    const $ = id => document.getElementById(id);
    let mounted = null, busy = false, lastServer = null;
    function fail(error) {
        $('error').hidden = false;
        $('error').textContent = String(error.message || error);
        console.error(error);
    }
    window.addEventListener('error', e => fail(e.error || `Resource failed: ${e.target.src || e.target.href || e.message}`), true);
    window.addEventListener('unhandledrejection', e => fail(e.reason));
    function component(type) {
        const c = window.PCjs?.components.find(c => c.type === type && c.id.startsWith('cad.'));
        if (!c || !c.flags.ready) throw new Error(`${type} is not ready; wait for the machine to load.`);
        return c;
    }
    const binding = (type, name) => {
        const b = component(type).bindings[name];
        if (!b) throw new Error(`PCjs ${type}.${name} control unavailable`);
        return b;
    };
    const running = () => /halt|stop|pause/i.test(binding('CPU', 'run').textContent);
    async function get(url) {
        const r = await fetch(url, {cache: 'no-store'});
        if (!r.ok) throw new Error(`HTTP ${r.status} ${url}: ${await r.text()}`);
        return r;
    }
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
    function status() {
        const components = window.PCjs?.components.filter(c => c.id.startsWith('cad.')) || [];
        const ready = components.some(c => c.type === 'CPU') && components.every(c => c.flags.ready);
        return {ready, running: ready ? running() : false,
            speed: ready ? binding('CPU', 'setSpeed').textContent : null,
            mounted, busy, mouseCaptured: !!document.pointerLockElement, server: lastServer,
            components: components.map(c => ({id: c.id, type: c.type, ready: c.flags.ready}))};
    }
    async function inject(text, literal) {
        if (busy) throw new Error('A keyboard operation is already in progress');
        const k = component('Keyboard');
        if (!running()) throw new Error('Run the CPU before typing');
        if (typeof text !== 'string' || !text.length || text.length > 4096 || /[^\x01-\x7f]/.test(text))
            throw new Error('Supply 1–4096 ASCII characters');
        busy = true;
        try {
            if (!k.exports.type.call(k, literal ? text.replaceAll('$', '$$') : text, 150))
                throw new Error('PCjs rejected keyboard input');
            await new Promise((resolve, reject) => {
                const timer = setTimeout(() => reject(new Error('Keyboard injection timed out (CPU paused?)')), 120000);
                const done = () => { clearTimeout(timer); resolve(); };
                if (k.exports.wait.call(k, done)) done();
            });
        } finally { busy = false; }
    }
    const api = window.cadDebug = {
        status,
        run() { if (!running()) binding('CPU', 'run').click(); return status(); },
        pause() { if (running()) binding('CPU', 'run').click(); return status(); },
        reset() { if (busy) throw new Error('Wait for keyboard input to complete'); binding('CPU', 'reset').click(); return status(); },
        focus() { const c = document.querySelector('#cad .pcjs-overlay') || document.querySelector('#cad canvas'); if (!c) throw new Error('Display not ready'); c.focus(); },
        captureMouse() { binding('Video', 'lockPointer').click(); },
        releaseMouse() { if (document.pointerLockElement) document.exitPointerLock(); },
        speed() { binding('CPU', 'setSpeed').click(); return status(); },
        type: text => inject(text, true),
        keys: tokens => inject(tokens, false),
        async mountLatest() {
            const form = binding('FDC', 'mountDisk');
            const drives = binding('FDC', 'listDrives');
            drives.value = '0';
            drives.dispatchEvent(new Event('change', {bubbles: true}));
            const response = await get(`/api/disk?t=${Date.now()}`);
            const bytes = await response.arrayBuffer();
            const digest = await crypto.subtle.digest('SHA-256', bytes);
            const sha256 = Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('');
            const transfer = new DataTransfer();
            transfer.items.add(new File([bytes], `win31c-${sha256.slice(0, 12)}.img`, {type: 'application/octet-stream'}));
            const input = form.querySelector('input[type=file]');
            if (!input) throw new Error('PCjs original file input missing');
            input.files = transfer.files;
            input.dispatchEvent(new Event('change', {bubbles: true}));
            form.requestSubmit();
            // Disk FileReader/mount is asynchronous. Observe the original disk list:
            // PCjs adds the local disk only once the load callback succeeds.
            const disks = binding('FDC', 'listDisks');
            const end = Date.now() + 15000;
            while (!Array.from(disks.options).some(o => o.value === input.files[0].name)) {
                if (Date.now() > end) throw new Error('PCjs did not complete local disk mount; inspect native controls');
                await sleep(100);
            }
            mounted = {drive: 'A:', bytes: bytes.byteLength, sha256};
            api.focus();
            return mounted;
        },
        async launchHouse() {
            if (!mounted) throw new Error('Mount latest A: first, then select Program Manager');
            // injectKeys releases active modifiers after the following normal key.
            await api.keys('$altf$10r$10');
            await api.type('A:\\HOUSE.EXE\r');
            api.focus();
        },
        async launchCAD({uiTest = false} = {}) {
            if (!mounted) throw new Error('Mount the CAD build first, then select Program Manager');
            await api.keys('$altf$10r$10');
            await api.type(`A:\\CAD.EXE${uiTest ? ' /UITEST' : ''}\r`);
            api.focus();
        },
        screenshot() {
            const canvas = document.querySelector('#cad canvas');
            if (!canvas) throw new Error('Display not ready');
            const link = document.createElement('a');
            link.download = `win31c-${new Date().toISOString().replaceAll(':', '-')}.png`;
            link.href = canvas.toDataURL('image/png'); link.click();
            return link.href;
        }
    };
    document.querySelectorAll('[data-action]').forEach(button => button.addEventListener('click', async () => {
        try {
            $('error').hidden = true;
            if (button.dataset.action === 'typeText') await api.type($('text').value);
            else await api[button.dataset.action]();
        } catch (error) { fail(error); }
    }));
    async function poll() {
        try {
            lastServer = await (await get('/api/status')).json();
            const s = status();
            $('status').textContent = `${s.ready ? (s.running ? 'Running' : 'Paused') : 'Bootstrapping'} · CPU ${s.speed || '—'} · mouse ${s.mouseCaptured ? 'captured (Esc releases)' : 'free'} · latest disk ${lastServer.disk.sha256?.slice(0, 12) || 'missing'} · A: ${mounted?.sha256.slice(0, 12) || 'not mounted'}`;
            if (Object.keys(lastServer.asset_errors).length) fail(JSON.stringify(lastServer.asset_errors, null, 2));
            const warning = document.querySelector('.machine-warning');
            if (warning?.textContent.includes('Error:')) fail(warning.textContent);
        } catch (error) { fail(error); }
        setTimeout(poll, 2000);
    }
    if (typeof window.embedPCx86 !== 'function') fail('PCjs failed to load. Check server console and reload.');
    else window.embedPCx86('cad', '/machine.xml', '/machines/pcx86/xsl/components.xsl');
    poll();
})();
