import base64, sys
wasm, out = sys.argv[1], sys.argv[2]
b64 = base64.b64encode(open(wasm, 'rb').read()).decode()
tpl = open(__file__.replace('make_page.py', 'page.html'), encoding='utf-8').read()
open(out, 'w', encoding='utf-8').write(tpl.replace('__WASM_BASE64__', b64))
