// ============================================================================
//  zenith::crew::mountCrewDashboard —— 内嵌 Web 仪表盘（M13.4）
//
//  单文件 SPA（零外部依赖，暗色主题）挂载在 GET /crew/dashboard：
//    - 任务列表 + 新建任务表单
//    - 任务详情：7 角色实时状态卡、SSE 事件流、产物库、投票记录、人工放行
//  数据全部来自 mountCrewApi 的 REST/SSE 端点——本文件只提供静态 HTML。
// ============================================================================
#pragma once

#include <string>

#include "zenith/http/http_server.hpp"

namespace zenith::crew {

inline const std::string& crewDashboardHtml() {
    static const std::string kHtml = R"zhtml(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Zenith Crew · 多 Agent 协作仪表盘</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--fg:#e6edf3;--dim:#8b949e;
--acc:#58a6ff;--ok:#3fb950;--warn:#d29922;--err:#f85149;--vote:#bc8cff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.5 -apple-system,"Segoe UI",Roboto,"PingFang SC","Microsoft YaHei",sans-serif}
header{display:flex;align-items:center;gap:10px;padding:10px 20px;
background:var(--panel);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:9}
header h1{font-size:16px;margin:0}header .logo{color:var(--acc);font-weight:700}
#conn{font-size:11px;color:var(--dim)}#conn.on{color:var(--ok)}
main{max-width:1200px;margin:0 auto;padding:20px}
a{color:var(--acc);text-decoration:none}
button{background:#21262d;color:var(--fg);border:1px solid var(--border);
border-radius:6px;padding:6px 14px;cursor:pointer;font-size:13px}
button:hover{border-color:var(--acc)}button.primary{background:#1f6feb;border-color:#1f6feb}
button.warn{background:#9e6a03;border-color:#9e6a03}
.badge{display:inline-block;padding:1px 8px;border-radius:10px;font-size:11px;
border:1px solid var(--border);color:var(--dim)}
.badge.done{color:var(--ok);border-color:var(--ok)}
.badge.failed{color:var(--err);border-color:var(--err)}
.badge.awaiting_approval{color:var(--warn);border-color:var(--warn);animation:pulse 1.2s infinite}
.badge.running{color:var(--acc);border-color:var(--acc)}
@keyframes pulse{50%{opacity:.45}}
table{width:100%;border-collapse:collapse;margin-top:12px}
td,th{padding:8px 10px;border-bottom:1px solid var(--border);text-align:left;font-size:13px}
th{color:var(--dim);font-weight:600}
.panel{background:var(--panel);border:1px solid var(--border);border-radius:8px;
padding:14px 16px;margin-bottom:16px}
.panel h2{margin:0 0 10px;font-size:14px;color:var(--dim);font-weight:600}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:10px}
.agent{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:10px 12px}
.agent .role{font-weight:700;font-size:13px}
.agent .st{float:right;font-size:11px}
.st.idle{color:var(--dim)}.st.working{color:var(--acc)}.st.done{color:var(--ok)}.st.error{color:var(--err)}
.agent .dt{color:var(--dim);font-size:12px;margin-top:4px;word-break:break-all;
max-height:3.1em;overflow:hidden}
#events{max-height:340px;overflow-y:auto;font:12px/1.6 Consolas,monospace}
.ev{padding:3px 8px;border-left:2px solid var(--border);margin:2px 0}
.ev .who{color:var(--acc)}.ev .ph{color:var(--vote);font-size:11px}
.ev.vote{border-color:var(--vote)}.ev.artifact{border-color:var(--ok)}
.ev.error{border-color:var(--err)}.ev .t{color:var(--dim);font-size:11px}
pre.art{background:#0d1117;border:1px solid var(--border);border-radius:6px;padding:10px;
overflow:auto;max-height:260px;font-size:12px;white-space:pre-wrap}
.vote-card{border:1px solid var(--border);border-radius:8px;padding:10px 12px;margin:8px 0}
.vote-card .q{font-weight:600}.vote-card .w{color:var(--ok)}
.muted{color:var(--dim)}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
textarea,input[type=text]{background:#0d1117;color:var(--fg);border:1px solid var(--border);
border-radius:6px;padding:6px 10px;font-size:13px;width:100%}
textarea{min-height:64px;resize:vertical}
form.panel .row{margin-top:8px}
</style>
</head>
<body>
<header><span class="logo">▲ Zenith</span><h1>SDLC Crew 协作仪表盘</h1>
<span id="conn">○ 未连接</span><span class="muted" id="crumb"></span></header>
<main id="view">加载中…</main>
<script>
'use strict';
const $=s=>document.querySelector(s);
const esc=s=>String(s==null?'':s).replace(/[&<>"']/g,
c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const phaseBadge=p=>'<span class="badge '+esc(p)+'">'+esc(p)+'</span>';

/* ---------- 任务列表 ---------- */
async function listView(){
  document.title='Zenith Crew · 任务列表';
  $('#crumb').textContent='';
  $('#view').innerHTML=
   '<form class="panel" id="nf"><h2>新建协作任务</h2>'+
   '<textarea id="brief" placeholder="需求简述，例：做一个短链接服务，含 API 与落地页"></textarea>'+
   '<div class="row"><input type="text" id="model" placeholder="模型（留空=后端默认）" style="width:240px">'+
   '<label class="muted"><input type="checkbox" id="appr"> 交付前人工审批</label>'+
   '<button class="primary" type="submit">开工</button></div></form>'+
   '<div class="panel"><h2>任务</h2><table id="tb"><thead><tr>'+
   '<th>ID</th><th>需求</th><th>阶段</th><th>事件</th><th>产物</th><th>投票</th><th>状态</th></tr>'+
   '</thead><tbody></tbody></table></div>';
  $('#nf').onsubmit=async e=>{
    e.preventDefault();
    const body={brief:$('#brief').value};
    if($('#model').value) body.config={model:$('#model').value,require_approval:$('#appr').checked};
    else if($('#appr').checked) body.config={require_approval:true};
    const r=await fetch('/crew/tasks',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify(body)});
    const j=await r.json().catch(()=>({}));
    if(j.task_id) location.hash='#/task/'+j.task_id;
    else alert('创建失败：'+(j&&j.error?j.error:r.status));
  };
  const r=await fetch('/crew/tasks');const j=await r.json();
  const tb=$('#tb tbody');tb.innerHTML='';
  (j.tasks||[]).slice().reverse().forEach(t=>{
    const tr=document.createElement('tr');
    tr.innerHTML='<td><a href="#/task/'+esc(t.id)+'">'+esc(t.id.slice(0,8))+'</a></td>'+
     '<td>'+esc((t.brief||'').slice(0,60))+'</td><td>'+phaseBadge(t.phase)+'</td>'+
     '<td class="muted">'+t.event_count+'</td><td class="muted">'+t.artifact_count+'</td>'+
     '<td class="muted">'+t.vote_count+'</td>'+
     '<td>'+(t.running?'<span class="badge running">运行中</span>'
       :(t.phase==='failed'?'<span class="badge failed">失败</span>':'<span class="badge done">已结束</span>'))+'</td>';
    tb.appendChild(tr);
  });
  if(!(j.tasks||[]).length) tb.innerHTML='<tr><td colspan="7" class="muted">暂无任务</td></tr>';
  $('#conn').textContent='● 已连接';$('#conn').className='on';
}

/* ---------- 任务详情 ---------- */
let es=null,pollTimer=null;
async function detailView(id){
  document.title='Zenith Crew · '+id.slice(0,8);
  $('#crumb').textContent='/ '+id;
  $('#view').innerHTML=
   '<div class="row" style="margin-bottom:14px"><a href="#">&larr; 任务列表</a>'+
   '<span id="phase"></span><span id="runst" class="muted"></span>'+
   '<span id="cost" class="muted"></span>'+
   '<button id="cancelBtn" style="display:none">取消任务</button>'+
   '<button id="resumeBtn" style="display:none">断点恢复</button></div>'+
   '<div class="panel"><h2>角色状态</h2><div class="grid" id="agents"></div></div>'+
   '<div class="panel" id="apprPanel" style="display:none">'+
   '<h2>人工审批门禁</h2><div class="row"><span class="muted">任务等待放行</span>'+
   '<input type="text" id="apprComment" placeholder="审批评论（可选，入事件流）" style="width:320px">'+
   '<button class="warn" id="approveBtn">批准继续</button></div></div>'+
   '<div class="panel"><h2>实时事件 <span class="muted" id="evcount"></span></h2><div id="events"></div></div>'+
   '<div class="panel"><h2>投票记录</h2><div id="votes"></div></div>'+
   '<div class="panel"><h2>产物库</h2><div id="arts"></div></div>';
  $('#approveBtn').onclick=async()=>{
    await fetch('/crew/tasks/'+id+'/approve',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({comment:$('#apprComment').value})});
    refreshBoard();
  };
  $('#cancelBtn').onclick=async()=>{
    await fetch('/crew/tasks/'+id+'/cancel',{method:'POST'});
    refreshBoard();
  };
  $('#resumeBtn').onclick=async()=>{
    await fetch('/crew/tasks/'+id+'/resume',{method:'POST'});
    refreshBoard();
  };

  async function refreshBoard(){
    const r=await fetch('/crew/tasks/'+id+'/board');
    if(!r.ok){$('#phase').innerHTML='<span class="badge failed">任务不存在</span>';return;}
    const b=await r.json();
    $('#phase').innerHTML=phaseBadge(b.phase);
    $('#runst').textContent=b.running?'● 运行中':'○ 已结束';
    $('#cost').textContent=b.cost_usd>0
      ?('$'+b.cost_usd.toFixed(4)+' · '+(b.tokens_in+b.tokens_out)+' tok'):'';
    $('#apprPanel').style.display=b.phase==='awaiting_approval'?'':'none';
    $('#cancelBtn').style.display=b.running?'':'none';
    $('#resumeBtn').style.display=b.resumable?'':'none';
    const g=$('#agents');g.innerHTML='';
    (b.agents||[]).forEach(a=>{
      g.insertAdjacentHTML('beforeend',
       '<div class="agent"><span class="st '+esc(a.state)+'">● '+esc(a.state)+'</span>'+
       '<div class="role">'+esc(a.role)+'</div><div class="dt">'+esc(a.detail)+'</div></div>');
    });
  }
  async function refreshMeta(){
    const [va,vt]=await Promise.all([fetch('/crew/tasks/'+id+'/artifacts'),
                                     fetch('/crew/tasks/'+id+'/votes')]);
    const ja=await va.json(),jv=await vt.json();
    const vd=$('#votes');vd.innerHTML='';
    (jv.votes||[]).forEach(v=>{
      const rows=(v.votes||[]).map(x=>esc(x.agent)+' → <b>'+esc(x.choice)+'</b>').join(' · ');
      vd.insertAdjacentHTML('beforeend','<div class="vote-card"><div class="q">'+
       esc(v.question)+'</div><div class="muted" style="font-size:12px">'+rows+'</div>'+
       '<div>结论：<span class="w">'+esc(v.winner)+'</span> <span class="muted">'+
       esc(v.reason||'')+'</span></div></div>');
    });
    if(!(jv.votes||[]).length) vd.innerHTML='<span class="muted">暂无</span>';
    const ad=$('#arts');ad.innerHTML='';
    (ja.artifacts||[]).forEach((a,i)=>{
      ad.insertAdjacentHTML('beforeend','<div style="margin-bottom:10px">'+
       '<div class="row"><b>'+esc(a.type)+'</b><span class="muted">v'+a.version+
       ' · '+esc(a.agent)+'</span><a href="javascript:void 0" class="art-toggle" data-i="'+i+
       '">展开/收起</a></div><pre class="art" style="display:none">'+esc(a.content)+'</pre></div>');
    });
    ad.querySelectorAll('.art-toggle').forEach(el=>{
      el.onclick=()=>{const p=el.parentElement.nextElementSibling;
        p.style.display=p.style.display==='none'?'':'none';};
    });
    if(!(ja.artifacts||[]).length) ad.innerHTML='<span class="muted">暂无</span>';
  }

  const quietTypes={'token':1,'webhook':1};
  function appendEvent(e){
    const box=$('#events');if(!box)return;
    if(quietTypes[e.type]&&box.children.length>60)return; // token 流限流
    const line='<div class="ev '+esc(e.type)+'"><span class="t">#'+e.seq+'</span> '+
     '<span class="ph">['+esc(e.phase)+']</span> <span class="who">'+esc(e.agent)+
     '</span> '+esc(e.detail)+'</div>';
    box.insertAdjacentHTML('beforeend',line);
    box.scrollTop=box.scrollHeight;
    $('#evcount').textContent='· '+box.children.length;
  }

  refreshBoard();refreshMeta();
  if(es)es.close();
  es=new EventSource('/crew/tasks/'+id+'/events/stream');
  es.addEventListener('crew',ev=>{const e=JSON.parse(ev.data);appendEvent(e);});
  es.addEventListener('end',()=>{if(es){es.close();es=null;}
    refreshBoard();refreshMeta();});
  es.onerror=()=>{/* 终态后服务端关流——转入轮询兜底 */};
  clearInterval(pollTimer);
  pollTimer=setInterval(refreshBoard,3000);
}

/* ---------- 路由 ---------- */
function route(){
  const m=location.hash.match(/^#\/task\/([A-Za-z0-9_-]+)/);
  if(m)detailView(m[1]);else{if(es){es.close();es=null;}clearInterval(pollTimer);listView();}
}
window.addEventListener('hashchange',route);
route();
</script>
</body>
</html>)zhtml";
    return kHtml;
}

/// 挂载内嵌仪表盘：GET /crew/dashboard（与 mountCrewApi 同一 Router）
inline void mountCrewDashboard(http::Router& r) {
    r.get("/crew/dashboard", [](http::Context& c) {
        c.html(200, crewDashboardHtml());
    });
}

} // namespace zenith::crew
