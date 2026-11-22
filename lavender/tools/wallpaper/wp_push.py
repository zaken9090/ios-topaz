import os,subprocess,time,sys
os.chdir(r'C:\Users\Vasyl\Desktop\Claude\Projects\Android-iOS')
LOG='dump/usblog-m51.txt'
env=dict(os.environ,MSYS_NO_PATHCONV='1')
done=open(LOG,encoding='utf-8',errors='replace').read()
files=[os.path.join(r,f) for r,_,fs in os.walk('dump/wp') for f in fs]
bad=[]
for f in files:
    rel=os.path.relpath(f,'dump/wp').replace(os.sep,'/')
    dst='/Library/Wallpaper/Collections/'+rel
    if 'put '+dst+': ' in done and any(', write 0' in x[:80] for x in done.split('put '+dst+': ')[1:]): continue
    for attempt in range(4):
        pos=os.path.getsize(LOG)
        subprocess.run([sys.executable,'ios-lavender/lavender/tools/usblog.py','--put',f,dst],env=env,capture_output=True)
        res=None
        for _ in range(60):
            time.sleep(0.5)
            with open(LOG,encoding='utf-8',errors='replace') as h:
                h.seek(pos); t=h.read()
            if 'put refused' in t: res='busy'; break
            k='put '+dst+': '
            if k in t and ', write ' in t.rsplit(k,1)[1][:80]:
                res=t.rsplit(k,1)[1][:80]; break
        if res=='busy' or res is None: time.sleep(2); continue
        break
    ok=res and 'write 0' in res
    if not ok: bad.append((rel,res))
print(len(files),'files, bad',len(bad))
for b in bad: print(b)
