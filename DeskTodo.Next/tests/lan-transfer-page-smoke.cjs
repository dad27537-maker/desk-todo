// Run after DeskTodoLanTransferSmoke has exported the actual served page.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

async function main() {
    const page = fs.readFileSync(process.argv[2] ||
        path.join(__dirname, '../build/Release/lan-transfer-smoke-page.html'), 'utf8');
    const script = page.match(/<script>([\s\S]*?)<\/script>/)[1];
    const elements = new Map();
    function element() {
        return {
            value: '', textContent: '', innerHTML: '', disabled: false, dataset: {},
            replaceChildren() {}, append() {}, focus() {}, select() { this.selected = true; },
            setSelectionRange(start, end) { this.selection = [start, end]; }
        };
    }
    const get = id => {
        if (!elements.has(id)) elements.set(id, element());
        return elements.get(id);
    };
    let remote = { version: '1', text: '电脑中文 😀\r\n<script>example</script>' };
    let post = '', requests = 0, offline = false, failPost = false, legacyCopy = true;
    const navigator = {};
    const window = { isSecureContext: false };
    vm.runInNewContext(script, {
        document: { querySelector: get, createElement: element,
            execCommand(command) { assert.equal(command, 'copy'); return legacyCopy; } },
        navigator, window, TextEncoder, setTimeout() {},
        async fetch(url, options) {
            if (offline) throw Error('offline');
            if (options?.method === 'POST') {
                post = options.body;
                requests++;
                return { ok: !failPost, async text() { return '模拟发送失败'; } };
            }
            return { ok: true, async json() { return url.startsWith('/text?') ? { ...remote } : []; } };
        }
    });
    await new Promise(setImmediate);
    assert.equal(get('#computerText').value, remote.text);
    assert.equal(get('#computerText').innerHTML, '');
    assert.equal(get('#copyText').disabled, false);

    await get('#sendText').onclick();
    assert.equal(requests, 0);
    get('#phoneText').value = '文'.repeat(400000);
    await get('#sendText').onclick();
    assert.equal(requests, 0, 'limit should count UTF-8 bytes');
    get('#phoneText').value = '手机复制 🐱\n第二行';
    await get('#sendText').onclick();
    assert.equal(post, get('#phoneText').value);
    assert.match(get('#sendTextStatus').textContent, /已发送/);
    failPost = true;
    await get('#sendText').onclick();
    assert.match(get('#sendTextStatus').textContent, /发送失败/);
    assert.equal(get('#sendText').disabled, false);
    assert.equal(get('#phoneText').value, '手机复制 🐱\n第二行');

    await get('#copyText').onclick();
    assert.equal(get('#computerText').selected, true);
    assert.match(get('#textStatus').textContent, /已复制/);
    legacyCopy = false;
    await get('#copyText').onclick();
    assert.match(get('#textStatus').textContent, /长按/);
    window.isSecureContext = true;
    let copied = '';
    navigator.clipboard = { async writeText(text) { copied = text; } };
    await get('#copyText').onclick();
    assert.equal(copied, remote.text);
    navigator.clipboard.writeText = async () => { throw Error('denied'); };
    await get('#copyText').onclick();
    assert.match(get('#textStatus').textContent, /长按/);

    offline = true;
    await get('#refreshText').onclick();
    assert.match(get('#textStatus').textContent, /无法连接/);
    assert.equal(get('#computerText').value, remote.text, 'disconnect should retain received text');
    offline = false;
    await get('#refreshText').onclick();
    assert.match(get('#textStatus').textContent, /已连接/);
    remote = { version: '2', text: '' };
    await get('#refreshText').onclick();
    assert.equal(get('#computerText').value, '');
    assert.equal(get('#copyText').disabled, true);
    assert.equal(get('#phoneText').value, '手机复制 🐱\n第二行', 'polling overwrote draft');
    console.log('LAN phone page passed: text send/receive, UTF-8 limit, clipboard fallback, disconnect, clear, draft preservation.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
