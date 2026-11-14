#!/usr/bin/env python3
"""Preserve complete source-build logs and expose the first compiler diagnostic."""
import pathlib,re,shutil,sys
root=pathlib.Path(sys.argv[1]).resolve(); dest=root/'build-failure-logs';dest.mkdir(exist_ok=True)
paths=sorted((root/'UniXNU/out/sheng').glob('*/*/logs/*.log'))
if (root/'metal-frontend-check.log').is_file():paths.insert(0,root/'metal-frontend-check.log')
pattern=re.compile(r'(?:fatal )?error:|undefined symbols|Undefined symbols|Traceback \(most recent call last\)')
for path in paths:
    rel=path.relative_to(root); target=dest/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
    lines=path.read_text(errors='replace').splitlines();hits=[i for i,line in enumerate(lines) if pattern.search(line)]
    if hits:
        print('\n=== First error context:',rel,'===')
        first=hits[0];print('\n'.join(f'{i+1}: {lines[i]}' for i in range(max(0,first-5),min(len(lines),first+61))))
print('Saved',len(paths),'complete logs in',dest)
