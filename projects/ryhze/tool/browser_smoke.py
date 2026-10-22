"""Local-only browser smoke test. Requires the built web app and Playwright."""
import json, time
from pathlib import Path
from playwright.sync_api import sync_playwright
out = Path('docs/evidence')
out.mkdir(parents=True, exist_ok=True)
report = {'mode': 'Flutter web release', 'browser': 'Chromium', 'native_performance': 'not measured', 'runs': []}
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path='/usr/bin/chromium', headless=True, args=['--no-sandbox'])
    for name, w, h in [('desktop', 1440, 1000), ('phone', 390, 844), ('deck', 1280, 800)]:
        page = browser.new_page(viewport={'width':w, 'height':h}, device_scale_factor=1)
        errors=[]; network=[]
        page.on('pageerror', lambda e: errors.append(str(e)))
        page.on('response', lambda r: network.append({'url':r.url, 'status':r.status}))
        start=time.monotonic()
        page.goto('http://127.0.0.1:8765/', wait_until='networkidle')
        page.locator('flt-semantics-placeholder').click(force=True)
        page.get_by_role('button', name='Search Ryhze').wait_for(state='visible')
        ready=round((time.monotonic()-start)*1000)
        page.screenshot(path=str(out/f'{name}-games.png'))
        page.get_by_role('button', name='Search Ryhze').click()
        page.get_by_role('textbox', name='Search titles').fill('larcenous')
        page.get_by_role('button', name='Explore Larcenous Driftscape', exact=True).click()
        page.get_by_role('button', name='Back', exact=True).wait_for()
        page.screenshot(path=str(out/f'{name}-detail.png'))
        page.keyboard.press('Escape')
        page.get_by_role('button', name='Films', exact=True).click()
        page.screenshot(path=str(out/f'{name}-films.png'))
        report['runs'].append({'viewport':name,'width':w,'height':h,'navigation_to_search_control_ms':ready,'page_errors':errors,'responses':network})
        page.close()
    browser.close()
(out/'browser-smoke.json').write_text(json.dumps(report,indent=2))
print(json.dumps({'runs':[{'viewport':x['viewport'],'ready_ms':x['navigation_to_search_control_ms'],'errors':x['page_errors']} for x in report['runs']]},indent=2))
