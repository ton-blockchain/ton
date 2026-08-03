#!/usr/bin/env python3
"""Render the bench-spam blocks.csv as a self-contained interactive HTML chart.

Plots transactions-per-block against block seqno: total txs and the jetton
transfers (matched externals) we actually care about. Output is one standalone
HTML file (data embedded, vanilla-canvas chart, no CDN) — open it anywhere.

Usage:
  python benchmark/plot_blocks.py /mnt/bench/net-bench/spam/blocks.csv
  python benchmark/plot_blocks.py /mnt/bench/net-bench            # finds spam/blocks.csv
  python benchmark/plot_blocks.py <dir> -o /tmp/run.html
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import webbrowser


def resolve_csv(path: str) -> str:
    if os.path.isdir(path):
        for cand in (os.path.join(path, "blocks.csv"), os.path.join(path, "spam", "blocks.csv")):
            if os.path.isfile(cand):
                return cand
        sys.exit(f"no blocks.csv under {path}")
    if not os.path.isfile(path):
        sys.exit(f"not found: {path}")
    return path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    _ = ap.add_argument("csv", help="blocks.csv, the spam dir, or the net dir")
    _ = ap.add_argument("-o", "--out", help="output HTML path (default: blocks.html next to the csv)")
    _ = ap.add_argument("--open", action="store_true", help="open the result in a browser")
    _ = ap.add_argument("--title", default=None, help="chart title")
    args = ap.parse_args()

    csv_path = resolve_csv(args.csv)
    out_path = args.out or os.path.join(os.path.dirname(os.path.abspath(csv_path)), "blocks.html")

    rows: list[dict[str, int]] = []
    with open(csv_path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append(
                {
                    "seqno": int(r["seqno"]),
                    "utime": int(r["utime"]),
                    "t_ms": int(r["observed_at_unix_ms"]),
                    "n_txs": int(r["n_txs"]),
                    "n_jetton": int(r["n_ext_matched"]),
                }
            )
    if not rows:
        sys.exit(f"no rows in {csv_path}")
    rows.sort(key=lambda x: x["seqno"])

    n = len(rows)
    txs = [r["n_txs"] for r in rows]
    jet = [r["n_jetton"] for r in rows]
    title = args.title or os.path.basename(os.path.dirname(os.path.abspath(csv_path))) or "blocks"

    summary = {
        "blocks": n,
        "seqno_lo": rows[0]["seqno"],
        "seqno_hi": rows[-1]["seqno"],
        "txs_total": sum(txs),
        "jetton_total": sum(jet),
        "txs_mean": round(sum(txs) / n, 1),
        "jetton_mean": round(sum(jet) / n, 1),
        "txs_max": max(txs),
        "jetton_max": max(jet),
        "empty_blocks": sum(1 for v in txs if v == 0),
    }

    html = _TEMPLATE.replace("__TITLE__", json.dumps(title)).replace(
        "__DATA__", json.dumps(rows, separators=(",", ":"))
    ).replace("__SUMMARY__", json.dumps(summary))

    with open(out_path, "w") as f:
        _ = f.write(html)
    print(f"wrote {out_path}  ({n} blocks, seqno {summary['seqno_lo']}..{summary['seqno_hi']}, "
          f"mean {summary['txs_mean']} tx/blk, {summary['jetton_mean']} jetton/blk)")
    if args.open:
        _ = webbrowser.open("file://" + os.path.abspath(out_path))
    return 0


_TEMPLATE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>bench-spam blocks</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; background:#0e1117; color:#c9d1d9; font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace; }
  header { padding:14px 18px 6px; }
  h1 { font-size:15px; margin:0 0 8px; font-weight:600; }
  #stats { display:flex; flex-wrap:wrap; gap:6px 18px; color:#8b949e; }
  #stats b { color:#c9d1d9; font-weight:600; }
  #legend { padding:4px 18px; display:flex; gap:16px; }
  #legend span { cursor:pointer; user-select:none; }
  #legend .off { opacity:.35; text-decoration:line-through; }
  #legend .ctl { cursor:default; color:#8b949e; margin-left:6px; }
  #legend .ctl b { color:#c9d1d9; }
  #meanToggle { cursor:pointer; }
  #win { width:48px; background:#161b22; color:#c9d1d9; border:1px solid #30363d; border-radius:4px; padding:1px 4px;
         font:inherit; }
  .sw { display:inline-block; width:11px; height:11px; border-radius:2px; vertical-align:middle; margin-right:5px; }
  #wrap { padding:4px 12px 18px; }
  .chart { margin-top:10px; }
  .cap { color:#8b949e; font-size:12px; margin:0 0 2px 2px; }
  canvas.cv { width:100%; display:block; cursor:crosshair; }
  #tip { position:fixed; pointer-events:none; background:#161b22; border:1px solid #30363d; border-radius:6px;
         padding:6px 9px; font-size:12px; display:none; white-space:nowrap; box-shadow:0 4px 14px #0008; }
  #tip .s { color:#8b949e; }
</style></head>
<body>
<header>
  <h1 id="title"></h1>
  <div id="stats"></div>
</header>
<div id="legend">
  <span data-k="n_txs"><i class="sw" style="background:#58a6ff"></i>total txs</span>
  <span data-k="n_jetton"><i class="sw" style="background:#3fb950"></i>jetton transfers</span>
  <span class="ctl" id="meanToggle">rolling mean: <b id="meanState">on</b></span>
  <span class="ctl">window <input id="win" type="number" min="1" max="9999" value="15"> blk</span>
</div>
<div id="wrap">
  <div class="chart"><div class="cap">transactions per block — vs seqno</div><canvas class="cv" id="c1"></canvas></div>
  <div class="chart"><div class="cap">throughput — TPS vs time (rolling = trailing-window rate; raw = per-block, spiky on fetch jitter)</div><canvas class="cv" id="c2"></canvas></div>
</div>
<div id="tip"></div>
<script>
const TITLE=__TITLE__, DATA=__DATA__, SUM=__SUMMARY__;
const tip=document.getElementById("tip");
const PAD={l:56,r:14,t:12,b:34};
const VIS={n_txs:true, n_jetton:true};   // series visibility, shared across charts
let showMean=true, winSize=15;

document.getElementById("title").textContent = TITLE + " — block transactions & throughput";
const order=[["blocks","blocks"],["seqno_lo","seqno"],["seqno_hi","..seqno"],["txs_mean","mean tx/blk"],
  ["jetton_mean","mean jetton/blk"],["txs_max","max tx"],["jetton_max","max jetton"],["empty_blocks","empty blocks"]];
document.getElementById("stats").innerHTML = order.map(([k,l])=>`<div>${l}: <b>${SUM[k]}</b></div>`).join("");

// ---- derived series ----
const N=DATA.length;
const seqno=DATA.map(d=>d.seqno), tms=DATA.map(d=>d.t_ms);
const tsec=tms.map(t=>(t-tms[0])/1000);                       // seconds since first block
const dt=DATA.map((d,i)=> i? Math.max(1e-3,(tms[i]-tms[i-1])/1000) : 0); // per-block interval
if(N>1) dt[0]=dt[1];
const ntx=DATA.map(d=>d.n_txs), njet=DATA.map(d=>d.n_jetton);
const jtps=njet.map((v,i)=> v/dt[i]);                         // per-block instantaneous TPS (spiky)
const txps=ntx.map((v,i)=> v/dt[i]);
// Trailing W-block rate: transfers in the window / its true time span. Immune to
// the single-block dt collapse that makes per-block instantaneous TPS spike when
// the watcher fetches two blocks back-to-back. This is the honest throughput line.
function windowedRate(counts){
  const out=new Array(N);
  for(let i=0;i<N;i++){
    const lo=Math.max(0,i-winSize);
    let s=0; for(let j=lo+1;j<=i;j++) s+=counts[j];
    const span=(tms[i]-tms[lo])/1000;
    out[i]= span>1e-3 ? s/span : counts[i]/dt[i];
  }
  return out;
}

// Centered moving average over an array; window shrinks at the edges (unbiased ends).
function movingAvg(arr,w){
  const half=(w/2)|0, out=new Array(arr.length);
  for(let i=0;i<arr.length;i++){
    const a=Math.max(0,i-half), b=Math.min(arr.length-1,i+half);
    let s=0; for(let j=a;j<=b;j++) s+=arr[j];
    out[i]=s/(b-a+1);
  }
  return out;
}
function niceMax(v){ if(v<=0) return 1; const p=Math.pow(10,Math.floor(Math.log10(v))); const n=v/p;
  const m=n<=1?1:n<=2?2:n<=5?5:10; return m*p; }
// Robust ceiling so a single tiny-interval spike can't squash the whole y-axis.
function robustMax(arrays){ let all=[]; for(const a of arrays) all=all.concat(a);
  if(!all.length) return 1; all.sort((x,y)=>x-y);
  return all[Math.min(all.length-1, Math.floor(all.length*0.98))]; }
function nearest(xs,x){ let lo=0,hi=xs.length-1;
  while(lo<hi){ const m=(lo+hi)>>1; if(xs[m]<x) lo=m+1; else hi=m; }
  if(lo>0 && Math.abs(xs[lo-1]-x)<Math.abs(xs[lo]-x)) lo--; return lo; }

// ---- generic chart over a shared x axis ----
// cfg: {xs, xlabel, xfmt, robust, series:[{key,color,values,label}], tip(i)->html}
function makeChart(id, cfg){
  const cv=document.getElementById(id), ctx=cv.getContext("2d");
  let geom=null, meansByKey={};   // latest mean/rate arrays, for the tooltip
  function vis(){ return cfg.series.filter(s=>VIS[s.key]); }
  function draw(){
    const dpr=window.devicePixelRatio||1;
    const w=cv.parentElement.clientWidth;
    const h=Math.max(220, Math.min(440, Math.round(w*0.34)));
    cv.style.height=h+"px"; cv.width=Math.round(w*dpr); cv.height=Math.round(h*dpr);
    ctx.setTransform(dpr,0,0,dpr,0,0); ctx.clearRect(0,0,w,h);
    const x0=PAD.l, x1=w-PAD.r, y0=h-PAD.b, y1=PAD.t;
    const xLo=cfg.xs[0], xHi=cfg.xs[cfg.xs.length-1];
    const V=vis();
    const rawVals=V.map(s=>s.values);
    // The "mean" line is a windowed rate when the series defines one, else a plain
    // moving average. The rate form (sum/Δt) is the spike-proof one for chart 2.
    const meanVals=V.map(s=> s.meanFn ? s.meanFn() : movingAvg(s.values,winSize));
    meansByKey={}; V.forEach((s,si)=>{ meansByKey[s.key]=meanVals[si]; });
    let ymax;
    if(showMean){ let mm=1; for(const m of meanVals) for(const v of m) if(v>mm) mm=v; ymax=niceMax(mm*1.12); }
    else { const r=cfg.robust ? robustMax(rawVals) : Math.max(1,...rawVals.map(a=>Math.max(...a))); ymax=niceMax(r); }
    const sx=x=>x0+(x1-x0)*(x-xLo)/Math.max(1e-9,(xHi-xLo));
    const sy=v=>y0+(y1-y0)*(Math.min(v,ymax)/ymax);

    ctx.strokeStyle="#21262d"; ctx.fillStyle="#8b949e"; ctx.lineWidth=1; ctx.font="11px ui-monospace,monospace";
    ctx.textAlign="right"; ctx.textBaseline="middle";
    for(let i=0;i<=5;i++){ const v=ymax*i/5, y=sy(v);
      ctx.beginPath(); ctx.moveTo(x0,y); ctx.lineTo(x1,y); ctx.stroke();
      ctx.fillText(v>=1000?(v/1000).toFixed(1)+"k":Math.round(v), x0-7, y); }
    ctx.textAlign="center"; ctx.textBaseline="top";
    for(let i=0;i<=6;i++){ const x=xLo+(xHi-xLo)*i/6; ctx.fillText(cfg.xfmt(x), sx(x), y0+6); }
    ctx.fillText(cfg.xlabel, (x0+x1)/2, y0+19);

    ctx.save(); ctx.beginPath(); ctx.rect(x0,y1,x1-x0,y0-y1); ctx.clip();
    V.forEach((s,si)=>{
      // raw line: bold when no mean, faint underlay when mean is shown
      ctx.strokeStyle=s.color; ctx.globalAlpha=showMean?0.28:1; ctx.lineWidth=1.2; ctx.beginPath();
      for(let i=0;i<N;i++){ const X=sx(cfg.xs[i]), Y=sy(s.values[i]); i?ctx.lineTo(X,Y):ctx.moveTo(X,Y); }
      ctx.stroke();
      if(showMean){ ctx.globalAlpha=1; ctx.lineWidth=2; ctx.beginPath();
        const m=meanVals[si];
        for(let i=0;i<N;i++){ const X=sx(cfg.xs[i]), Y=sy(m[i]); i?ctx.lineTo(X,Y):ctx.moveTo(X,Y); }
        ctx.stroke(); }
    });
    ctx.restore(); ctx.globalAlpha=1;
    geom={x0,x1,y0,y1,xLo,xHi,sx,sy};
  }
  cv.addEventListener("mousemove",e=>{
    if(!geom) return;
    const r=cv.getBoundingClientRect(), mx=e.clientX-r.left;
    if(mx<geom.x0-2||mx>geom.x1+2){ tip.style.display="none"; return; }
    const x=geom.xLo+(geom.xHi-geom.xLo)*(mx-geom.x0)/Math.max(1,(geom.x1-geom.x0));
    const i=nearest(cfg.xs,x);
    draw();
    const X=geom.sx(cfg.xs[i]);
    ctx.strokeStyle="#484f58"; ctx.lineWidth=1; ctx.beginPath(); ctx.moveTo(X,geom.y1); ctx.lineTo(X,geom.y0); ctx.stroke();
    for(const s of vis()){ const Y=geom.sy(s.values[i]);
      ctx.fillStyle=s.color; ctx.beginPath(); ctx.arc(X,Y,3,0,7); ctx.fill(); }
    tip.innerHTML=cfg.tip(i, meansByKey); tip.style.display="block";
    let tx=e.clientX+14; if(tx+tip.offsetWidth>window.innerWidth) tx=e.clientX-tip.offsetWidth-14;
    tip.style.left=tx+"px"; tip.style.top=(e.clientY+14)+"px";
  });
  cv.addEventListener("mouseleave",()=>{ tip.style.display="none"; draw(); });
  return {draw};
}

const fmtClock=i=>new Date(tms[i]).toLocaleTimeString();
const chart1=makeChart("c1",{
  xs:seqno, xlabel:"seqno", xfmt:x=>Math.round(x), robust:false,
  series:[{key:"n_txs",color:"#58a6ff",values:ntx},
          {key:"n_jetton",color:"#3fb950",values:njet}],
  tip:(i,m)=>`<div>seqno <b>${seqno[i]}</b> <span class="s">${fmtClock(i)}</span></div>`+
         (VIS.n_txs?`<div style="color:#58a6ff">txs ${ntx[i]} <span class="s">(avg ${m.n_txs[i].toFixed(0)})</span></div>`:"")+
         (VIS.n_jetton?`<div style="color:#3fb950">jetton ${njet[i]} <span class="s">(avg ${m.n_jetton[i].toFixed(0)})</span></div>`:""),
});
const chart2=makeChart("c2",{
  xs:tsec, xlabel:"seconds", xfmt:x=>x.toFixed(0)+"s", robust:true,
  series:[{key:"n_txs",color:"#58a6ff",values:txps,meanFn:()=>windowedRate(ntx)},
          {key:"n_jetton",color:"#3fb950",values:jtps,meanFn:()=>windowedRate(njet)}],
  tip:(i,m)=>`<div><b>${tsec[i].toFixed(1)}s</b> <span class="s">seqno ${seqno[i]}, ${winSize}-blk avg</span></div>`+
         (VIS.n_txs?`<div style="color:#58a6ff">${m.n_txs[i].toFixed(0)} tx/s</div>`:"")+
         (VIS.n_jetton?`<div style="color:#3fb950">${m.n_jetton[i].toFixed(0)} jTPS</div>`:""),
});

function redraw(){ chart1.draw(); chart2.draw(); }
document.querySelectorAll("#legend span[data-k]").forEach(el=>{
  el.onclick=()=>{ const k=el.dataset.k; VIS[k]=!VIS[k]; el.classList.toggle("off",!VIS[k]); redraw(); };
});
document.getElementById("meanToggle").onclick=()=>{
  showMean=!showMean; document.getElementById("meanState").textContent=showMean?"on":"off"; redraw(); };
document.getElementById("win").addEventListener("input",e=>{
  const v=parseInt(e.target.value,10); if(v>=1){ winSize=v; redraw(); } });
window.addEventListener("resize",redraw);
redraw();
</script>
</body></html>
"""


if __name__ == "__main__":
    raise SystemExit(main())
