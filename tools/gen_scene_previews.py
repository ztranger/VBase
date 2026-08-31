# Превью сцен VBase: парсит .scene, строит навсетку, гоняет BFS поля потока от ядра
# (как сервер) и рисует топ-даун карту с зоной достижимости и маршрутом. Пишет один
# самодостаточный HTML (тема-aware, SVG через CSS-переменные).
import math, os, html, sys

# Пути — относительно репозитория (tools/ в корне). Вывод: аргумент 1 или <repo>/scene_previews.html.
_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(_REPO, "app", "src", "main", "assets")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_REPO, "scene_previews.html")

KDX = [1,-1,0,0, 1,1,-1,-1]
KDZ = [0,0,1,-1, 1,-1,1,-1]
CLEAR = 0.3            # kEnemyRadius (клиренс для статичных коллайдеров)
MIN_OVL = 0.2         # kMinOverlap

def cellOf(w, cell): return math.floor(w/cell)

def parse_scene(path):
    d = {"cell":2.0, "arena":11.0, "colliders":[], "buildings":[], "spawners":[], "player":None}
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split('#',1)[0].strip()
            if not line: continue
            t = line.split()
            k = t[0]
            def num(i): return float(t[i])
            if k=="grid":
                # grid cell C arena A
                for i,w in enumerate(t):
                    if w=="cell": d["cell"]=num(i+1)
                    if w=="arena": d["arena"]=num(i+1)
            elif k=="collider" and len(t)>=11 and t[1]=="box":
                # collider box center x y z half hx hy hz
                cx,cy,cz = num(3),num(4),num(5); hx,hy,hz = num(7),num(8),num(9)
                d["colliders"].append((cx,cy,cz,hx,hy,hz))
            elif k in ("core","tower","generator","storage","spawner"):
                # <kind> pos x y z
                pi = t.index("pos")
                x,z = float(t[pi+1]), float(t[pi+3])
                if k=="spawner": d["spawners"].append((x,z))
                else: d["buildings"].append((k,x,z))
            elif k=="player":
                if "pos" in t:
                    pi=t.index("pos"); d["player"]=(float(t[pi+1]), float(t[pi+3]))
    return d

def build_nav(d):
    cell=d["cell"]; A=d["arena"]
    lo=cellOf(-A,cell); hi=cellOf(A,cell)
    W=hi-lo+1
    blocked=[[False]*W for _ in range(W)]
    def raster(cx,cz,hx,hz,pad):
        minx,maxx=cx-hx-pad,cx+hx+pad; minz,maxz=cz-hz-pad,cz+hz+pad
        cx0,cx1=cellOf(minx,cell),cellOf(maxx-1e-4,cell)
        cz0,cz1=cellOf(minz,cell),cellOf(maxz-1e-4,cell)
        for gz in range(cz0,cz1+1):
            z0=gz*cell; oz=min(maxz,z0+cell)-max(minz,z0)
            if oz<MIN_OVL: continue
            for gx in range(cx0,cx1+1):
                x0=gx*cell; ox=min(maxx,x0+cell)-max(minx,x0)
                if ox<MIN_OVL: continue
                lx,lz=gx-lo,gz-lo
                if 0<=lx<W and 0<=lz<W: blocked[lz][lx]=True
    # статичные коллайдеры (кроме пола) с клиренсом
    for (cx,cy,cz,hx,hy,hz) in d["colliders"]:
        if cy+hy<=0.05: continue
        raster(cx,cz,hx,hz,CLEAR)
    # футпринты зданий (кроме спавнера) — без клиренса
    for (k,x,z) in d["buildings"]:
        raster(x,z,cell*0.5,cell*0.5,0.0)
    return lo,W,blocked

def bfs(d,lo,W,blocked):
    cell=d["cell"]
    INF=10**9
    dist=[[INF]*W for _ in range(W)]
    q=[]
    # источники — клетки ядер
    cores=[(x,z) for (k,x,z) in d["buildings"] if k=="core"]
    for (x,z) in cores:
        lx,lz=cellOf(x,cell)-lo,cellOf(z,cell)-lo
        if 0<=lx<W and 0<=lz<W and dist[lz][lx]!=0:
            dist[lz][lx]=0; q.append((lx,lz))
    head=0
    while head<len(q):
        lx,lz=q[head]; head+=1
        for dcnt in range(8):
            nx,nz=lx+KDX[dcnt],lz+KDZ[dcnt]
            if not(0<=nx<W and 0<=nz<W): continue
            if dist[nz][nx]!=INF: continue
            if blocked[nz][nx]: continue
            if dcnt>=4:  # диагональ: не резать угол
                if blocked[lz][lx+KDX[dcnt]] or blocked[lz+KDZ[dcnt]][lx]: continue
            dist[nz][nx]=dist[lz][lx]+1
            q.append((nx,nz))
    return dist,INF

def route(d,lo,W,dist,INF,start):
    cell=d["cell"]
    lx,lz=cellOf(start[0],cell)-lo,cellOf(start[1],cell)-lo
    if not(0<=lx<W and 0<=lz<W) or dist[lz][lx]==INF: return []
    pts=[]; guard=0
    while dist[lz][lx]>0 and guard<10000:
        guard+=1
        pts.append(((lx+lo+0.5)*cell,(lz+lo+0.5)*cell))
        best=dist[lz][lx]; bx,bz=lx,lz
        for dcnt in range(8):
            nx,nz=lx+KDX[dcnt],lz+KDZ[dcnt]
            if 0<=nx<W and 0<=nz<W and dist[nz][nx]<best:
                best=dist[nz][nx]; bx,bz=nx,nz
        if (bx,bz)==(lx,lz): break
        lx,lz=bx,bz
    pts.append(((lx+lo+0.5)*cell,(lz+lo+0.5)*cell))
    return pts

# ---------- SVG ----------
SZ=340; PAD=14
def svg_for(d):
    cell=d["cell"]; A=d["arena"]
    lo,W,blocked=build_nav(d)
    dist,INF=bfs(d,lo,W,blocked)
    # достижимость спавнеров
    reachable_sp=0
    paths=[]
    for sp in d["spawners"]:
        lx,lz=cellOf(sp[0],cell)-lo,cellOf(sp[1],cell)-lo
        ok = 0<=lx<W and 0<=lz<W and dist[lz][lx]!=INF
        if ok:
            reachable_sp+=1
            paths.append(route(d,lo,W,dist,INF,sp))
    reach_cells=sum(1 for r in dist for v in r if v!=INF)
    free_cells=sum(1 for r in blocked for b in r if not b)
    reach_pct=int(100*reach_cells/max(1,free_cells))
    solvable = reachable_sp>0
    # мир [-R,R] -> svg [PAD,SZ-PAD]
    R=A
    def sx(wx): return PAD+(wx+R)/(2*R)*(SZ-2*PAD)
    def sy(wz): return PAD+(R-wz)/(2*R)*(SZ-2*PAD)  # z вверх
    def rect(x0,x1,z0,z1,cls,extra=""):
        X=sx(x0); Y=sy(z1); Wd=sx(x1)-sx(x0); Hd=sy(z0)-sy(z1)
        return f'<rect x="{X:.1f}" y="{Y:.1f}" width="{Wd:.1f}" height="{Hd:.1f}" class="{cls}" {extra}/>'
    e=[]
    e.append(f'<rect x="{PAD}" y="{PAD}" width="{SZ-2*PAD}" height="{SZ-2*PAD}" class="floor"/>')
    # достижимые клетки (заливка)
    for lz in range(W):
        for lx in range(W):
            if dist[lz][lx]!=INF:
                x0=(lx+lo)*cell; z0=(lz+lo)*cell
                e.append(rect(x0,x0+cell,z0,z0+cell,"reach"))
    # стены-препятствия (не граница)
    for (cx,cy,cz,hx,hy,hz) in d["colliders"]:
        if cy+hy<=0.05: continue
        if max(hx,hz)>A*0.4: continue   # граница арены — рисуем рамкой, не стеной
        e.append(rect(cx-hx,cx+hx,cz-hz,cz+hz,"wall"))
    # маршруты
    for p in paths:
        if len(p)>=2:
            pts=" ".join(f"{sx(x):.1f},{sy(z):.1f}" for (x,z) in p)
            e.append(f'<polyline points="{pts}" class="route"/>')
    # здания
    for (k,x,z) in d["buildings"]:
        if k=="core":
            e.append(f'<circle cx="{sx(x):.1f}" cy="{sy(z):.1f}" r="7" class="core"/>')
        elif k=="tower":
            e.append(rect(x-0.7,x+0.7,z-0.7,z+0.7,"tower"))
        else:
            e.append(rect(x-0.7,x+0.7,z-0.7,z+0.7,"bld"))
    # спавнеры
    for (x,z) in d["spawners"]:
        cx,cy=sx(x),sy(z)
        e.append(f'<polygon points="{cx:.1f},{cy-6:.1f} {cx+6:.1f},{cy+5:.1f} {cx-6:.1f},{cy+5:.1f}" class="spawn"/>')
    # игрок
    if d["player"]:
        e.append(f'<circle cx="{sx(d["player"][0]):.1f}" cy="{sy(d["player"][1]):.1f}" r="4" class="player"/>')
    svg=f'<svg viewBox="0 0 {SZ} {SZ}" class="map" role="img">{"".join(e)}</svg>'
    return svg, dict(solvable=solvable, W=W, reach_pct=reach_pct,
                     spawners=len(d["spawners"]), reach_sp=reachable_sp,
                     walls=sum(1 for r in blocked for b in r if b))

# ---------- манифест + сборка ----------
def read_manifest():
    out=[]
    with open(os.path.join(ASSETS,"config","scenes.cfg"),encoding="utf-8") as f:
        for raw in f:
            line=raw.split('#',1)[0].strip()
            if not line: continue
            p=line.split(None,1)
            out.append((p[0], p[1].strip() if len(p)>1 else p[0]))
    return out

cards=[]
for path,name in read_manifest():
    full=os.path.join(ASSETS, path.replace("/", os.sep))
    if not os.path.exists(full):
        print("skip missing", path); continue
    d=parse_scene(full)
    svg,st=svg_for(d)
    badge = ('<span class="badge ok">путь к ядру есть</span>' if st["solvable"]
             else '<span class="badge warn">ядро запечатано → прогрыз</span>')
    stat=(f'{st["W"]}×{st["W"]} клеток · {st["walls"]} стен · '
          f'достижимо {st["reach_pct"]}% · спавнеров {st["reach_sp"]}/{st["spawners"]}')
    cards.append(f'''<article class="card">
      <div class="mapwrap">{svg}</div>
      <div class="meta">
        <h2>{html.escape(name)}</h2>
        {badge}
        <p class="stat">{stat}</p>
        <p class="path">{html.escape(path)}</p>
      </div>
    </article>''')
    print(f"  {path}: solvable={st['solvable']} reach={st['reach_pct']}% spawners={st['reach_sp']}/{st['spawners']}")

TEMPLATE = '''<title>VBase Nav Scenes</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo:wght@500;600;700&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
:root{
  --ground:#eef1f2; --surface:#ffffff; --line:#d3dad9; --ink:#152420; --muted:#5d6b67;
  --accent:#1f9e8c; --reach:rgba(31,158,140,.16); --route:#f0a01e;
  --floor:#e7ecec; --wall:#243330; --core:#e8b53a; --tower:#5f7bb0; --bld:#5fa564;
  --spawn:#d9563a; --player:#2bb8c8; --ok:#1f9e8c; --warn:#c9852a;
}
:root:not([data-theme="light"]){}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){
  --ground:#0c1211; --surface:#111a18; --line:#22302d; --ink:#e6efec; --muted:#8ea39d;
  --accent:#39c4ae; --reach:rgba(57,196,174,.20); --route:#f2b03a;
  --floor:#16211e; --wall:#93a9a3; --core:#f0c24d; --tower:#8aa4d6; --bld:#7cc281;
  --spawn:#ef6f4f; --player:#3fd0e0; --ok:#39c4ae; --warn:#e0a24a;
}}
:root[data-theme="dark"]{
  --ground:#0c1211; --surface:#111a18; --line:#22302d; --ink:#e6efec; --muted:#8ea39d;
  --accent:#39c4ae; --reach:rgba(57,196,174,.20); --route:#f2b03a;
  --floor:#16211e; --wall:#93a9a3; --core:#f0c24d; --tower:#8aa4d6; --bld:#7cc281;
  --spawn:#ef6f4f; --player:#3fd0e0; --ok:#39c4ae; --warn:#e0a24a;
}
*{box-sizing:border-box}
body{margin:0;background:var(--ground);color:var(--ink);
  font-family:Archivo,system-ui,sans-serif;line-height:1.5;
  -webkit-font-smoothing:antialiased}
.wrap{max-width:1180px;margin:0 auto;padding:40px 24px 64px}
header.top{margin-bottom:28px}
.eyebrow{font-family:"IBM Plex Mono",monospace;font-size:12px;letter-spacing:.12em;
  text-transform:uppercase;color:var(--accent);margin:0 0 6px}
h1{font-size:clamp(28px,4vw,40px);font-weight:700;margin:0 0 8px;text-wrap:balance;letter-spacing:-.01em}
.lede{max-width:64ch;color:var(--muted);margin:0 0 20px}
.legend{display:flex;flex-wrap:wrap;gap:14px 20px;padding:14px 16px;background:var(--surface);
  border:1px solid var(--line);border-radius:12px;font-size:13px;font-family:"IBM Plex Mono",monospace}
.legend span{display:inline-flex;align-items:center;gap:7px;color:var(--muted)}
.sw{width:13px;height:13px;border-radius:3px;flex:none}
.sw.core{border-radius:50%;background:var(--core)} .sw.tower{background:var(--tower)}
.sw.bld{background:var(--bld)} .sw.spawn{background:var(--spawn)}
.sw.player{border-radius:50%;background:var(--player)} .sw.wall{background:var(--wall)}
.sw.reach{background:var(--reach);border:1px solid var(--accent)}
.sw.route{background:var(--route)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:20px;margin-top:26px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:14px;overflow:hidden;
  display:flex;flex-direction:column}
.mapwrap{background:var(--floor);border-bottom:1px solid var(--line)}
svg.map{display:block;width:100%;height:auto}
svg .floor{fill:var(--floor)}
svg .reach{fill:var(--reach)}
svg .wall{fill:var(--wall)}
svg .route{fill:none;stroke:var(--route);stroke-width:2.4;stroke-linejoin:round;stroke-linecap:round;opacity:.95}
svg .core{fill:var(--core);stroke:var(--surface);stroke-width:1.5}
svg .tower{fill:var(--tower)} svg .bld{fill:var(--bld)}
svg .spawn{fill:var(--spawn)} svg .player{fill:var(--player);stroke:var(--surface);stroke-width:1}
.meta{padding:14px 16px 16px;display:flex;flex-direction:column;gap:7px}
.meta h2{font-size:17px;font-weight:600;margin:0;letter-spacing:-.01em}
.badge{align-self:flex-start;font-family:"IBM Plex Mono",monospace;font-size:11px;font-weight:500;
  padding:3px 9px;border-radius:999px;letter-spacing:.02em}
.badge.ok{background:color-mix(in srgb,var(--ok) 16%,transparent);color:var(--ok)}
.badge.warn{background:color-mix(in srgb,var(--warn) 18%,transparent);color:var(--warn)}
.stat{font-family:"IBM Plex Mono",monospace;font-size:12px;color:var(--muted);margin:0;
  font-variant-numeric:tabular-nums}
.path{font-family:"IBM Plex Mono",monospace;font-size:11.5px;color:var(--accent);margin:0;opacity:.85}
footer{margin-top:36px;color:var(--muted);font-size:13px;max-width:64ch}
footer code{font-family:"IBM Plex Mono",monospace;background:var(--surface);padding:2px 6px;
  border-radius:5px;border:1px solid var(--line)}
</style>
<div class="wrap">
  <header class="top">
    <p class="eyebrow">VBase · pathfinding scenes</p>
    <h1>Карты навигации</h1>
    <p class="lede">Топ-даун разбор сцен-сценариев. Заливкой показана зона, откуда поле
      потока ведёт к ядру (куда реально дойдут рашеры), линией — маршрут спавнер→ядро.
      Расчёт — тем же BFS, что на сервере (8 направлений, без срезания углов, клиренс капсулы).</p>
    <div class="legend">
      <span><i class="sw core"></i>ядро</span>
      <span><i class="sw spawn"></i>спавнер</span>
      <span><i class="sw tower"></i>башня</span>
      <span><i class="sw bld"></i>генератор/хранилище</span>
      <span><i class="sw player"></i>игрок</span>
      <span><i class="sw wall"></i>стена</span>
      <span><i class="sw reach"></i>достижимо до ядра</span>
      <span><i class="sw route"></i>маршрут</span>
    </div>
  </header>
  <div class="grid">
    __CARDS__
  </div>
  <footer>Смотреть в клиенте: <code>run-desktop.cmd</code> → меню, раздел «Сцена» → выбрать →
    Host. «Ядро запечатано» означает, что прямого пути нет и рашеры прогрызают постройки.</footer>
</div>'''

htmlout = TEMPLATE.replace("__CARDS__", "\n".join(cards))
with open(OUT,"w",encoding="utf-8") as f:
    f.write(htmlout)
print("written", OUT, len(htmlout), "bytes")
