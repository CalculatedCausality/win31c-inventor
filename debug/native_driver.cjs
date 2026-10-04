/* Windows Node controls only a new localhost page in the dedicated Edge profile.
 * Transport is stdin/stdout, never an open network proxy. */
const readline = require('node:readline');
const {chromium} = require(process.argv[2]);
let browser, page, session;
const send = value => process.stdout.write(JSON.stringify(value) + '\n');

async function initialize(viewport) {
    browser = await chromium.connectOverCDP('http://127.0.0.1:9223');
    // Reuse only our localhost app. Do not select/read extension or user tabs.
    const context = browser.contexts()[0];
    page = context.pages().find(p => ['http://localhost:8310/', 'http://127.0.0.1:8310/'].includes(p.url())) || await context.newPage();
    page.on('pageerror', error => send({event: 'pageerror', text: String(error)}));
    page.on('close', () => send({event: 'close'}));
    await page.setViewportSize(viewport || {width: 1100, height: 850});
    session = await page.context().newCDPSession(page);
    return true;
}

async function command(request) {
    const a = request.args || {};
    switch (request.method) {
    case 'init': return initialize(a.viewport);
    case 'goto': await page.goto(a.url); return true;
    case 'evaluate': {
        // Python Playwright accepts arrow-function source strings; Node's
        // API needs an actual function to invoke them with an argument.
        const functionSource = /^\s*(?:async\s+)?(?:function\b|\(?[\w\s,]*\)?\s*=>)/.test(a.expression);
        return functionSource ? page.evaluate(eval('(' + a.expression + ')'), a.argument) : page.evaluate(a.expression);
    }
    case 'wait': {
        const handle = await page.waitForFunction(a.expression, null, {timeout: a.timeout || 90000});
        await handle.dispose(); return true;
    }
    case 'front': await page.bringToFront(); return true;
    case 'cdp': return session.send(a.method, a.params || {});
    case 'closed': return page.isClosed();
    case 'waitClosed': await page.waitForEvent('close', {timeout: a.timeout}); return true;
    case 'disconnect': setImmediate(() => process.exit(0)); return true;
    default: throw new Error('Unknown bridge method');
    }
}

if (process.argv.includes('--inspect')) {
    initialize().then(async () => {
        send(await page.evaluate(() => ({title: document.title,
            status: window.cadDebug?.status(), error: document.getElementById('error')?.textContent})));
        await page.screenshot({path: 'C:/Users/mauco/Documents/GitHub/win31c/out/windows-visible.png'});
        process.exit(0);
    }).catch(error => {console.error(error); process.exit(1);});
} else if (process.argv.includes('--show-only')) {
    initialize().then(async () => {
        await page.goto('http://localhost:8310/');
        await page.evaluate("document.title='win31c — Windows CAD tests'");
        await page.bringToFront();
        send({visible: true}); process.exit(0);
    }).catch(error => {console.error(error); process.exit(1);});
} else {
    let queue = Promise.resolve();
    readline.createInterface({input: process.stdin}).on('line', line => {
        queue = queue.then(async () => {
            let request;
            try {
                request = JSON.parse(line);
                send({id: request.id, result: await command(request)});
            } catch (error) {
                send({id: request?.id, error: String(error.stack || error)});
            }
        });
    });
}
