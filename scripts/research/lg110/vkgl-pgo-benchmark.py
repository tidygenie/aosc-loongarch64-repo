#!/usr/bin/env python3
import json, os, pathlib, re, statistics, subprocess, sys, time

out = pathlib.Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
variants = {
    'installed': '/usr/share/vulkan/icd.d/vkgl_loonggpu.json',
    'pgo': '/home/houge/infra/gpu/candidates/vkgl-pgo-gcc16/vkgl_icd.json',
}
scenes = ('vertex', 'texture', 'shading')
rows = []
for round_index in range(6):
    order = ('installed', 'pgo') if round_index % 2 == 0 else ('pgo', 'installed')
    for variant in order:
        log = out / f'round-{round_index + 1}-{variant}.log'
        env = os.environ.copy()
        env.update({
            'XDG_RUNTIME_DIR': '/run/user/1000',
            'WAYLAND_DISPLAY': 'wayland-0',
            'VK_ICD_FILENAMES': variants[variant],
            'VK_DRIVER_FILES': variants[variant],
            'VK_INSTANCE_LAYERS': 'VK_LAYER_KHRONOS_validation',
        })
        env.pop('LD_PRELOAD', None)
        env.pop('VKGL_NO_DMABUF', None)
        cmd = ['vkmark', '--winsys', 'wayland', '--size', '640x480', '-p', 'fifo']
        for scene in scenes:
            cmd += ['-b', f'{scene}:duration=4']
        started = time.time()
        proc = subprocess.run(cmd, env=env, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=60)
        log.write_text(proc.stdout)
        if proc.returncode != 0:
            raise SystemExit(f'failed round={round_index + 1} variant={variant} rc={proc.returncode}')
        if 'LoongGPU LG110 (vkgl GLES2)' not in proc.stdout:
            raise SystemExit('wrong GPU')
        if re.search(r'llvmpipe|swrast|Validation Error:', proc.stdout, re.I):
            raise SystemExit('software renderer or validation error')
        values = {}
        for scene in scenes:
            match = re.search(rf'^\[{scene}\].*?FPS:\s+([0-9.]+)', proc.stdout, re.M)
            if not match:
                raise SystemExit(f'missing {scene} FPS')
            values[scene] = float(match.group(1))
        rows.append({'round': round_index + 1, 'variant': variant, 'seconds': time.time() - started,
                     'fps': values, 'log': log.name})
        print(json.dumps(rows[-1], sort_keys=True), flush=True)

summary = {'runs': rows, 'scenes': {}}
for scene in scenes:
    per_variant = {}
    for variant in variants:
        values = [r['fps'][scene] for r in rows if r['variant'] == variant]
        per_variant[variant] = {'values': values, 'median': statistics.median(values)}
    pairs = []
    for round_index in range(6):
        a = next(r['fps'][scene] for r in rows if r['round'] == round_index + 1 and r['variant'] == 'installed')
        b = next(r['fps'][scene] for r in rows if r['round'] == round_index + 1 and r['variant'] == 'pgo')
        pairs.append((b - a) / a * 100)
    summary['scenes'][scene] = {**per_variant, 'paired_percent': pairs,
                                'paired_median': statistics.median(pairs),
                                'positive_pairs': sum(x > 0 for x in pairs)}
(out / 'summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
print(json.dumps(summary['scenes'], indent=2, sort_keys=True))
