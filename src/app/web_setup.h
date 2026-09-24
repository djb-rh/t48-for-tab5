// The Wi-Fi setup page, served by the captive portal at /setup.
#pragma once

static const char kSetupPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>T48 Burner Wi-Fi</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--panel2:#21262d;--border:#30363d;--text:#e6edf3;--dim:#8b949e;--accent:#2f81f7;--good:#3fb950;--bad:#f85149}
@media (prefers-color-scheme:light){:root{--bg:#f6f8fa;--panel:#fff;--panel2:#eef1f4;--border:#d0d7de;--text:#1f2328;--dim:#59636e;--accent:#0969da;--good:#1a7f37;--bad:#cf222e}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:520px;margin:0 auto;padding:20px 16px}h1{font-size:22px;margin:0 0 4px}p{color:var(--dim);margin:0 0 16px}
.nets{background:var(--panel);border:1px solid var(--border);border-radius:10px;overflow:hidden;margin-bottom:16px}
.net{display:flex;justify-content:space-between;align-items:center;padding:12px 14px;border-bottom:1px solid var(--border);cursor:pointer}
.net:last-child{border-bottom:none}.net.sel{background:var(--panel2);box-shadow:inset 3px 0 var(--accent)}
.net small{color:var(--dim)}label{display:block;color:var(--dim);font-size:14px;margin:12px 0 4px}
input{width:100%;padding:11px 12px;font:inherit;color:var(--text);background:var(--panel);border:1px solid var(--border);border-radius:8px}
input:focus{outline:none;border-color:var(--accent)}.row{display:flex;gap:8px;margin-top:16px}
button{flex:1;padding:12px;font:inherit;border-radius:8px;border:1px solid var(--border);background:var(--panel2);color:var(--text);cursor:pointer}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
#status{margin-top:18px;padding:12px 14px;border-radius:8px;background:var(--panel);border:1px solid var(--border);display:none}
#status.good{border-color:var(--good)}#status.bad{border-color:var(--bad)}a{color:var(--accent)}
</style></head><body><main>
<h1>T48 Burner Wi-Fi</h1>
<p>Choose the network the Tab5 should join. It remembers it.</p>
<div class="nets" id="nets"><div class="net"><span>Looking for networks...</span></div></div>
<label for="ssid">Network name</label><input id="ssid" autocomplete="off" autocapitalize="none">
<label for="pass">Password</label><input id="pass" type="password" autocomplete="off">
<label style="display:flex;gap:8px;align-items:center;margin-top:10px"><input type="checkbox" id="show" style="width:auto"> Show password</label>
<div class="row"><button onclick="nets(1)">Scan again</button><button class="primary" onclick="join()">Join</button></div>
<div id="status"></div>
<p style="margin-top:22px">Or skip this and <a href="/files">manage files</a> over this hotspot.</p>
</main><script>
const $=s=>document.querySelector(s);
const esc=s=>s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
$('#show').onchange=e=>$('#pass').type=e.target.checked?'text':'password';
function status(t,cls){const s=$('#status');s.style.display='block';s.className=cls||'';s.innerHTML=t}
async function nets(rescan){if(rescan)$('#nets').innerHTML='<div class="net"><span>Scanning...</span></div>';
try{const l=await (await fetch('/api/nets'+(rescan?'?rescan=1':''))).json();
$('#nets').innerHTML=l.length?l.map(n=>'<div class="net" data-s="'+esc(n.ssid)+'"><span>'+esc(n.ssid)+'</span><small>'+(n.open?'open, ':'')+n.rssi+' dBm</small></div>').join(''):'<div class="net"><span>No networks found</span></div>';
document.querySelectorAll('.net[data-s]').forEach(d=>d.onclick=()=>{document.querySelectorAll('.net').forEach(x=>x.classList.remove('sel'));d.classList.add('sel');$('#ssid').value=d.dataset.s;$('#pass').focus()})}catch(e){}}
async function join(){const ssid=$('#ssid').value.trim();if(!ssid)return status('Choose or type a network name.','bad');
const b=new URLSearchParams({ssid:ssid,pass:$('#pass').value});
try{await fetch('/api/wifi',{method:'POST',body:b});}catch(e){}
status('Joining '+esc(ssid)+'... (this page may lose its connection for a moment)');poll(ssid,0)}
async function poll(ssid,n){try{const s=await (await fetch('/api/wifistate')).json();
if(s.state=='connected'){return status('Connected to <b>'+esc(s.ssid)+'</b>. The Tab5 is at <b>http://'+s.ip+'/</b><br>Put this phone back on '+esc(s.ssid)+' and open that address. This hotspot closes in a minute.','good')}
if(s.state=='failed'){return status('Could not join '+esc(ssid)+'. Check the password and try again.','bad')}}catch(e){}
if(n<30)setTimeout(()=>poll(ssid,n+1),2000);else status('No answer yet. Look at the Tab5 screen: it shows whether it joined.','bad')}
nets(0);
</script></body></html>
)HTML";
