// The browser side of the file manager, served at /.
#pragma once

static const char kWebPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>T48 for Tab5</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--panel2:#21262d;--border:#30363d;--text:#e6edf3;--dim:#8b949e;--accent:#2f81f7;--good:#3fb950;--bad:#f85149;--warn:#d29922}
@media (prefers-color-scheme:light){:root{--bg:#f6f8fa;--panel:#fff;--panel2:#eef1f4;--border:#d0d7de;--text:#1f2328;--dim:#59636e;--accent:#0969da;--good:#1a7f37;--bad:#cf222e;--warn:#9a6700}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
header{background:var(--panel);border-bottom:1px solid var(--border);padding:12px 16px;display:flex;gap:16px;align-items:baseline;flex-wrap:wrap}
header h1{font-size:18px;margin:0}#state{color:var(--dim);font-size:13px}
main{max-width:980px;margin:0 auto;padding:16px}
.bar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:12px}
.crumbs a{color:var(--accent);text-decoration:none;cursor:pointer}.crumbs span{color:var(--dim);margin:0 4px}
button,.btn{background:var(--panel2);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:6px 12px;font:inherit;cursor:pointer}
button:hover{border-color:var(--accent)}button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
button.danger:hover{border-color:var(--bad);color:var(--bad)}
#drop{border:2px dashed var(--border);border-radius:10px;padding:22px;text-align:center;color:var(--dim);margin-bottom:14px}
#drop.over{border-color:var(--accent);color:var(--text)}
table{width:100%;border-collapse:collapse;background:var(--panel);border:1px solid var(--border);border-radius:10px;overflow:hidden}
th,td{padding:8px 12px;border-bottom:1px solid var(--border);text-align:left}th{color:var(--dim);font-weight:500;font-size:13px}
tr:last-child td{border-bottom:none}td.size{color:var(--dim);white-space:nowrap;text-align:right;font-variant-numeric:tabular-nums}
td.name a{color:var(--text);text-decoration:none;cursor:pointer}td.name a.dir{color:var(--accent);font-weight:600}
td.acts{text-align:right;white-space:nowrap}td.acts button{padding:3px 8px;font-size:13px;margin-left:4px}
.cur{color:var(--good);font-size:12px;margin-left:8px}
#msg{min-height:22px;color:var(--dim);margin:8px 0}
.prog{height:6px;background:var(--panel2);border-radius:3px;overflow:hidden;margin-top:8px}.prog i{display:block;height:100%;background:var(--accent);width:0}
@media (max-width:600px){td.acts button{margin:2px 0 2px 4px}th.size,td.size{display:none}}
</style></head><body>
<header><h1>T48 for Tab5</h1><div id="state">...</div></header>
<main>
<div class="bar"><div class="crumbs" id="crumbs"></div><div style="flex:1"></div>
<button onclick="mkdir()">New folder</button>
<label class="btn primary">Upload files<input type="file" id="pick" multiple hidden></label></div>
<div id="drop">Drop ROM images here to upload them to this folder<div class="prog" id="prog" hidden><i></i></div></div>
<div id="msg"></div>
<table><thead><tr><th>Name</th><th class="size">Size</th><th></th></tr></thead><tbody id="rows"></tbody></table>
</main>
<script>
let dir='/burner/images',image='';
const $=s=>document.querySelector(s);
const fmt=n=>n<1048576?n.toLocaleString()+' bytes':(n/1048576).toFixed(1)+' MB';
const esc=s=>s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function msg(t,bad){const m=$('#msg');m.textContent=t;m.style.color=bad?'var(--bad)':'var(--dim)'}
async function api(path,opt){const r=await fetch(path,opt);if(!r.ok)throw new Error(await r.text()||r.statusText);return r}
async function state(){try{const s=await (await api('/api/state')).json();image=s.image;
$('#state').textContent='Chip: '+(s.part||'none')+'  |  Image: '+(s.image?s.image.split('/').pop():'none')+'  |  T48 '+(s.t48?'connected':'not found')+(s.busy?'  |  busy: '+s.job:'')}catch(e){}}
function crumbs(){const parts=dir.split('/').filter(Boolean);let h='',p='';
parts.forEach((x,i)=>{p+='/'+x;h+=(i?'<span>/</span>':'')+(i<1?esc(x):'<a data-p="'+esc(p)+'">'+esc(x)+'</a>')});
$('#crumbs').innerHTML=h;$('#crumbs').querySelectorAll('a').forEach(a=>a.onclick=()=>go(a.dataset.p))}
async function go(d){dir=d;crumbs();await load()}
async function load(){await state();try{const list=await (await api('/api/list?path='+encodeURIComponent(dir))).json();
list.sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name,undefined,{numeric:true}));
let h=dir!='/burner'?'<tr><td class="name"><a class="dir" data-up>..</a></td><td class="size"></td><td></td></tr>':'';
for(const f of list){const p=dir+'/'+f.name;
h+='<tr><td class="name">'+(f.dir?'<a class="dir" data-d="'+esc(p)+'">'+esc(f.name)+'/</a>':'<a href="/api/get?path='+encodeURIComponent(p)+'" download="'+esc(f.name)+'">'+esc(f.name)+'</a>'+('/sdcard'+p==image?'<span class="cur">current image</span>':''))+
'</td><td class="size">'+(f.dir?'':fmt(f.size))+'</td><td class="acts">'+
(f.dir?'':'<button data-use="'+esc(p)+'">Burn this</button>')+'<button data-ren="'+esc(p)+'">Rename</button><button class="danger" data-del="'+esc(p)+'">Delete</button></td></tr>'}
if(!list.length)h+='<tr><td colspan="3" style="color:var(--dim)">Empty folder</td></tr>';
$('#rows').innerHTML=h;
$('#rows').querySelectorAll('[data-d]').forEach(a=>a.onclick=()=>go(a.dataset.d));
$('#rows').querySelectorAll('[data-up]').forEach(a=>a.onclick=()=>go(dir.replace(/\/[^/]+$/,'')));
$('#rows').querySelectorAll('[data-use]').forEach(b=>b.onclick=()=>post('/api/use?path=',b.dataset.use,'Chosen on the Tab5: '));
$('#rows').querySelectorAll('[data-del]').forEach(b=>b.onclick=()=>{if(confirm('Delete '+b.dataset.del.split('/').pop()+'?'))post('/api/delete?path=',b.dataset.del,'Deleted ')});
$('#rows').querySelectorAll('[data-ren]').forEach(b=>b.onclick=()=>ren(b.dataset.ren));
}catch(e){msg(e.message,1)}}
async function post(u,p,t){try{await api(u+encodeURIComponent(p),{method:'POST'});msg(t+p.split('/').pop());load()}catch(e){msg(e.message,1)}}
async function ren(p){const n=prompt('New name',p.split('/').pop());if(!n)return;
try{await api('/api/rename?from='+encodeURIComponent(p)+'&to='+encodeURIComponent(dir+'/'+n),{method:'POST'});load()}catch(e){msg(e.message,1)}}
async function mkdir(){const n=prompt('Folder name');if(!n)return;post('/api/mkdir?path=',dir+'/'+n,'Made ')}
function put(file){return new Promise((ok,no)=>{const x=new XMLHttpRequest();
x.open('PUT','/api/put?path='+encodeURIComponent(dir+'/'+file.name));
x.upload.onprogress=e=>{$('#prog i').style.width=(e.loaded/e.total*100)+'%'};
x.onload=()=>x.status==200?ok():no(new Error(x.responseText||x.statusText));x.onerror=()=>no(new Error('upload failed'));x.send(file)})}
async function upload(files){$('#prog').hidden=false;let n=0;
for(const f of files){msg('Uploading '+f.name+' ('+(++n)+' of '+files.length+')...');
try{await put(f)}catch(e){msg(f.name+': '+e.message,1);$('#prog').hidden=true;return load()}}
$('#prog').hidden=true;msg('Uploaded '+files.length+' file'+(files.length>1?'s':''));load()}
$('#pick').onchange=e=>{upload([...e.target.files]);e.target.value=''};
const dz=$('#drop');['dragenter','dragover'].forEach(t=>dz.addEventListener(t,e=>{e.preventDefault();dz.classList.add('over')}));
['dragleave','drop'].forEach(t=>dz.addEventListener(t,e=>{e.preventDefault();dz.classList.remove('over')}));
dz.addEventListener('drop',e=>upload([...e.dataTransfer.files]));
go(dir);setInterval(state,3000);
</script></body></html>
)HTML";
