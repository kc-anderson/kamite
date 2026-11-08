# Stages: dict(label, g=gene motions, v=victim motions, align=(vec x, vec z, mode, f12[, 'unscaled']) or None,
#              frames=stage length (None: Gene's motions), touch=(first, last) frames whose touches count, or None)
# From the phases' step tables (Gene 0x41C350 for Cobra, victim 0x4439E0) and the SEQ events (attack window flags:
# 1 = the hit, 2 / 8 = knocked down, 0x10 = Cobra's release switch at 1549 frame 40 while the victim lives).
import math
PUNISH={
 'Suplex':  [dict(label='the throw',g=[333],v=[329],align=(-0.5842,0.1376,0,0.0,'unscaled'),touch=(0,38))],
 'Stinger': [dict(label='grab',g=[811],v=[805],align=(-0.182,1.179,0,0.0),touch=(0,9)),
             dict(label='the knees (mash loop)',g=[813],v=[807],align=(-0.234,1.215,1,0.0),touch=(0,20)),
             dict(label='the slam',g=[815],v=[809],align=(-0.254,1.169,1,0.0),touch=(0,35))],
 'Cobra':   [dict(label='grab',g=[1551],v=[1543],align=(0.338,-0.340,0,math.pi),touch=(0,31)),
             dict(label='the twist (mash loop)',g=[1553],v=[1545],align=None,touch=(0,20)),
             dict(label='the final pull',g=[1557],v=[1549],align=None,frames=41,touch=(0,40)),
             dict(label='release',g=[1555],v=[1547],align=None,touch=None)],
 'Spanking':[dict(label='over the knee',g=[1337],v=[1327],align=(0.303,0.585,0,0.0),touch=(0,22)),
             dict(label='the spanks (mash loop)',g=[1339],v=[1329],align=None,touch=(0,16)),
             dict(label='finish (launch at 21)',g=[1341],v=[1331,1333,1335],align=None,frames=24,touch=(0,21))],
}
