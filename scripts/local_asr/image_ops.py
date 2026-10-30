"""Offline two-axis scrolling screenshot alignment. No capture or input injection."""
from pathlib import Path
import json
import numpy as np

def require_axes(horizontal,vertical):
    if not isinstance(horizontal,bool) or not isinstance(vertical,bool) or not (horizontal or vertical):
        raise ValueError('长截图至少启用一个滚动方向')

def alignment(previous,current,horizontal=False,vertical=True):
    import cv2
    require_axes(horizontal,vertical)
    if previous.shape!=current.shape or previous.ndim!=3:raise ValueError('截图区域尺寸发生变化，请重新开始')
    if np.mean(np.abs(previous.astype(np.int16)-current.astype(np.int16)))<.6:return (0,0)
    gray=lambda im:cv2.cvtColor(im,cv2.COLOR_RGB2GRAY)
    orb=cv2.ORB_create(nfeatures=5000,fastThreshold=8)
    ka,da=orb.detectAndCompute(gray(previous),None);kb,db=orb.detectAndCompute(gray(current),None)
    if da is None or db is None:raise ValueError('画面特征不足，请缩小截图区域或减小滚动距离')
    matches=cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(db,da,k=2)
    good=[]
    for pair in matches:
        if len(pair)!=2:continue
        best,second=pair
        if best.distance>=second.distance*.7:continue
        old=np.asarray(ka[best.trainIdx].pt);new=np.asarray(kb[best.queryIdx].pt);delta=old-new
        # Discard fixed headers/toolbars instead of mistaking them for no scroll.
        if np.linalg.norm(delta)<5:continue
        if not horizontal and abs(delta[0])>4:continue
        if not vertical and abs(delta[1])>4:continue
        good.append((new,old))
    if len(good)<12:raise ValueError('相邻截图重叠不足，请回滚一部分后重新捕获')
    matrix,mask=cv2.estimateAffinePartial2D(np.float32([x[0] for x in good]),np.float32([x[1] for x in good]),method=cv2.RANSAC,ransacReprojThreshold=2)
    if matrix is None or mask is None or int(mask.sum())<10 or float(mask.mean())<.35:raise ValueError('无法可靠拼接，本帧未加入；请避开动画或固定侧栏')
    if abs(matrix[0,0]-1)>.012 or abs(matrix[1,1]-1)>.012 or abs(matrix[0,1])>.012 or abs(matrix[1,0])>.012:raise ValueError('检测到缩放或旋转，请保持页面缩放不变')
    dx=int(round(matrix[0,2])) if horizontal else 0;dy=int(round(matrix[1,2])) if vertical else 0
    h,w=previous.shape[:2]
    if abs(dx)>w*.85 or abs(dy)>h*.85:raise ValueError('滚动幅度过大，请保留至少 15% 重叠')
    return dx,dy

def stitch(frames,horizontal=False,vertical=True,max_pixels=64_000_000):
    require_axes(horizontal,vertical)
    if not frames:raise ValueError('没有截图')
    positions=[(0,0)];kept=[frames[0]];x=y=0
    for previous,current in zip(frames,frames[1:]):
        dx,dy=alignment(previous,current,horizontal,vertical);x+=dx;y+=dy
        if dx or dy:positions.append((x,y));kept.append(current)
    height,width=frames[0].shape[:2]
    left=min(x for x,y in positions);top=min(y for x,y in positions)
    right=max(x for x,y in positions)+width;bottom=max(y for x,y in positions)+height
    if (right-left)*(bottom-top)>max_pixels:raise ValueError('拼接结果超过 6400 万像素，请分段保存')
    canvas=np.full((bottom-top,right-left,3),255,dtype=np.uint8)
    # Exact aligned replacement avoids blurred text from alpha blending.
    for im,(x,y) in zip(kept,positions):canvas[y-top:y-top+height,x-left:x-left+width]=im
    return canvas,positions

def run_job(path):
    from PIL import Image,ImageOps
    path=Path(path);job=json.loads(path.read_text(encoding='utf8'))
    result=path.with_suffix('.result.json')
    try:
        files=job['frames']
        if not isinstance(files,list) or not 1<=len(files)<=60:raise ValueError('长截图限 60 帧，请分段处理')
        images=[]
        for name in files:
            im=Image.open(name)
            if im.width*im.height>16_000_000:raise ValueError('单帧超过 1600 万像素')
            images.append(np.asarray(ImageOps.exif_transpose(im).convert('RGB')))
        image,positions=stitch(images,job.get('horizontal',False),job.get('vertical',True))
        output=path.with_suffix('.png');Image.fromarray(image).save(output)
        data=dict(ok=True,path=str(output),width=image.shape[1],height=image.shape[0],frames=len(positions))
    except Exception as error:data=dict(ok=False,error=str(error)[:300])
    result.write_text(json.dumps(data,ensure_ascii=False),encoding='utf8')
