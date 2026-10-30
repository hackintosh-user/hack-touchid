const REPO="hackintosh-user/hack-touchid", CL_BRANCH="main";
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function inline(t){return esc(t).replace(/`([^`]+)`/g,"<code>$1</code>").replace(/\*\*([^*]+)\*\*/g,"<b>$1</b>").replace(/\[([^\]]+)\]\((https?:[^)\s]+)\)/g,'<a href="$2" target="_blank" rel="noopener">$1</a>');}
function md(src){
  let out="",list=false,code=false;
  for(const raw of src.split(/\r?\n/)){
    if(raw.startsWith("```")){out+=code?"</pre>":"<pre>";code=!code;continue}
    if(code){out+=esc(raw)+"\n";continue}
    const l=raw.trimEnd();
    if(/^\s*[-*] /.test(l)){if(!list){out+="<ul>";list=true}out+="<li>"+inline(l.replace(/^\s*[-*] /,""))+"</li>";continue}
    if(list){out+="</ul>";list=false}
    if(/^#{1,3} /.test(l)){out+="<h3>"+inline(l.replace(/^#+ /,""))+"</h3>";continue}
    if(/^-{3,}$/.test(l)){out+="<hr>";continue}
    if(l)out+="<p>"+inline(l)+"</p>";
  }
  return out+(list?"</ul>":"")+(code?"</pre>":"");
}
async function loadChangelog(el){
  try{
    const r=await fetch(`https://raw.githubusercontent.com/${REPO}/${CL_BRANCH}/CHANGELOG.md`);
    if(!r.ok)throw 0;
    el.innerHTML=md(await r.text());
  }catch(e){el.innerHTML=`<p class="muted">Couldn't load the changelog right now. <a href="https://github.com/${REPO}/blob/${CL_BRANCH}/CHANGELOG.md">Read it on GitHub</a>.</p>`}
}
function ago(d){const s=(Date.now()-new Date(d))/1e3;for(const[u,n]of[["d",86400],["h",3600],["m",60]])if(s>=n)return Math.floor(s/n)+u+" ago";return "just now"}
async function loadCommits(el){
  try{
    const br=await (await fetch(`https://api.github.com/repos/${REPO}/branches?per_page=20`)).json();
    if(!Array.isArray(br))throw 0;
    let html="";
    br.sort((a,b)=>(b.name===CL_BRANCH)-(a.name===CL_BRANCH));
    for(const b of br){
      const cs=await (await fetch(`https://api.github.com/repos/${REPO}/commits?sha=${encodeURIComponent(b.name)}&per_page=5`)).json();
      if(!Array.isArray(cs))throw 0;
      html+=`<div class="card" style="margin-bottom:14px"><h3><code>${esc(b.name)}</code> <a href="https://github.com/${REPO}/commits/${encodeURIComponent(b.name)}" style="font-weight:400;font-size:.85rem">all commits</a></h3>`+
      cs.map(c=>`<div class="commit"><code><a href="${c.html_url}" target="_blank" rel="noopener">${c.sha.slice(0,7)}</a></code><div>${esc(c.commit.message.split("\n")[0])}<small>${esc(c.commit.author.name)} · ${ago(c.commit.author.date)}</small></div></div>`).join("")+"</div>";
    }
    el.innerHTML=html||"<p class='muted'>No branches found.</p>";
  }catch(e){el.innerHTML=`<p class="muted">Couldn't load commits right now (GitHub may be rate limiting). <a href="https://github.com/${REPO}/commits">See them on GitHub</a>.</p>`}
}
document.querySelectorAll("[data-changelog]").forEach(loadChangelog);
document.querySelectorAll("[data-commits]").forEach(loadCommits);
(function(){
  const w=document.querySelector("nav .wrap");if(!w)return;
  const links=[...w.querySelectorAll("a:not(.brand)")];if(!links.length)return;
  const btn=document.createElement("button");btn.className="navbtn";btn.type="button";
  btn.setAttribute("aria-label","Menu");btn.setAttribute("aria-expanded","false");
  btn.innerHTML="<span></span><span></span><span></span>";
  const ov=document.createElement("div");ov.className="navov";
  const dr=document.createElement("aside");dr.className="navdr";
  const br=w.querySelector(".brand");if(br)dr.appendChild(br.cloneNode(true));
  const IC={
    "index.html":'<path d="M3 11l9-8 9 8"/><path d="M5 10v10h14V10"/><path d="M10 20v-6h4v6"/>',
    "docs.html":'<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/><path d="M9 13h6M9 17h6"/>',
    "guide.html":'<path d="M3 5.5A1.5 1.5 0 0 1 4.5 4H10a2 2 0 0 1 2 2v14a2 2 0 0 0-2-2H3z"/><path d="M21 5.5A1.5 1.5 0 0 0 19.5 4H14a2 2 0 0 0-2 2v14a2 2 0 0 1 2-2h7z"/>',
    "security.html":'<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
    "contribute.html":'<path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z"/>'
  };
  const GH='<path d="M12 .297c-6.63 0-12 5.373-12 12 0 5.303 3.438 9.8 8.205 11.385.6.113.82-.258.82-.577 0-.285-.01-1.04-.015-2.04-3.338.724-4.042-1.61-4.042-1.61C4.422 18.07 3.633 17.7 3.633 17.7c-1.087-.744.084-.729.084-.729 1.205.084 1.838 1.236 1.838 1.236 1.07 1.835 2.809 1.305 3.495.998.108-.776.417-1.305.76-1.605-2.665-.3-5.466-1.332-5.466-5.93 0-1.31.465-2.38 1.235-3.22-.135-.303-.54-1.523.105-3.176 0 0 1.005-.322 3.3 1.23.96-.267 1.98-.399 3-.405 1.02.006 2.04.138 3 .405 2.28-1.552 3.285-1.23 3.285-1.23.645 1.653.24 2.873.12 3.176.765.84 1.23 1.91 1.23 3.22 0 4.61-2.805 5.625-5.475 5.92.42.36.81 1.096.81 2.22 0 1.606-.015 2.896-.015 3.286 0 .315.21.69.825.57C20.565 22.092 24 17.592 24 12.297c0-6.627-5.373-12-12-12"/>';
  links.forEach(a=>{
    const c=a.cloneNode(true),h=a.getAttribute("href")||"";
    const key=Object.keys(IC).find(k=>h.endsWith(k));
    const svg=key?IC[key]:(h.includes("github.com")?GH:"");
    if(svg)c.insertAdjacentHTML("afterbegin",'<svg class="ni'+(key?'':' fill')+'" viewBox="0 0 24 24" aria-hidden="true">'+svg+'</svg>');
    dr.appendChild(c);
  });
  const set=o=>{dr.classList.toggle("open",o);ov.classList.toggle("open",o);btn.setAttribute("aria-expanded",o)};
  btn.addEventListener("click",()=>set(!dr.classList.contains("open")));
  ov.addEventListener("click",()=>set(false));
  dr.addEventListener("click",e=>{if(e.target.closest("a"))set(false)});
  document.addEventListener("keydown",e=>{if(e.key==="Escape")set(false)});
  w.parentElement.classList.add("js");document.body.classList.add("side");w.prepend(btn);document.body.append(ov,dr);
})();
