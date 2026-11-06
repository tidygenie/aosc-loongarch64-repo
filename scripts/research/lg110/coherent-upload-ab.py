#!/usr/bin/env python3
"""Alternating controlled persistent-coherent-upload benchmark."""
import argparse,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--base',required=True);p.add_argument('--candidate',required=True)
p.add_argument('--test',type=Path,default=Path('/tmp/lg110-coherent-upload/vktest-coherent-upload'))
p.add_argument('--rounds',type=int,default=5);p.add_argument('--frames',type=int,default=120)
p.add_argument('--draws',type=int,default=64);p.add_argument('--texture-size',type=int,default=256)
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
if min(a.rounds,a.frames,a.draws)<1 or a.texture_size not in (64,256,1024):p.error('invalid workload')
here=Path(__file__).resolve().parent
rows=[]
for rnd in range(1,a.rounds+1):
 for variant in (('base','candidate') if rnd%2 else ('candidate','base')):
  env=os.environ.copy();env.update({
   'XDG_RUNTIME_DIR':f'/run/user/{os.getuid()}','WAYLAND_DISPLAY':'wayland-0',
   'VK_ICD_FILENAMES':a.base if variant=='base' else a.candidate,
   'LG110_TEXTURE_VARIANT':'A','LG110_FRAME_LIMIT':str(a.frames),'LG110_FRAME_SLEEP_US':'0',
   'LG110_BENCHMARK':'1','LG110_DRAW_VERTEX_COUNT':'2','LG110_DRAWS_PER_FRAME':str(a.draws),
   'LG110_TEXTURE_SIZE':str(a.texture_size),'LG110_READBACK_FRAME':'0',
   'LG110_READBACK_PIXEL':f'/tmp/coherent-upload-{os.getpid()}-{rnd}-{variant}.rgba',
  })
  env['VK_DRIVER_FILES']=env['VK_ICD_FILENAMES']
  for key in ('LD_PRELOAD','VKGL_DEBUG','VKGL_NO_DMABUF','VK_INSTANCE_LAYERS'):env.pop(key,None)
  r=subprocess.run([str(a.test)],cwd=here,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
  m=re.search(r'BENCHMARK frames=(\d+) draws_per_frame=(\d+) texture_size=(\d+) elapsed_ms=([0-9.]+) fps=([0-9.]+) draws_per_second=([0-9.]+)',r.stdout)
  row={'round':rnd,'variant':variant,'rc':r.returncode,'draws_per_frame':a.draws,'texture_size':a.texture_size}
  if m:row.update(frames=int(m.group(1)),elapsed_ms=float(m.group(4)),fps=float(m.group(5)),draws_per_second=float(m.group(6)),pixels_valid='pixels_valid=1' in r.stdout)
  rows.append(row);print(json.dumps(row),flush=True)
  if r.returncode or not m or not row.get('pixels_valid'):raise SystemExit(r.stdout)
a.output.write_text(''.join(json.dumps(x)+'\n' for x in rows))
