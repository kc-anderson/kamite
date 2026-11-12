"""Bodies outside the humanoid groups (K, Nov 12). The punish motions drive bones by number, so on a non-human skeleton
they pose whatever bones carry those numbers. em23, the snake demon (body 4cd0aad5, 41 parts: a long spine 0-6 with the
head at 6, two pairs of arms 7-30, the tail after): Suplex plays as the game makes it; Spanking with the game's
placement and Gene's holding (left) arm raised 60 deg off his back (eased in over the grab's first 8 frames, out from
frame 12 of the finish to the launch at 21, spank_fat2's timing; arm in the back 11.7 -> 5.9 cm, spine only); Stinger
and Cobra Twist off (his own punish plays for those picks).

    python -I odd_bodies.py    (replaces the block at the end of resources/punish_fit.txt; emit7 appends it too)"""
import os
HERE=os.path.dirname(os.path.abspath(__file__))
FILE=os.path.join(HERE,'..','..','..','resources','punish_fit.txt')
MARK='# odd bodies (odd_bodies.py)'
SNAKE_RAISE=60.0
def gene(motion,keys):
    return 'gene %d %s'%(motion,' '.join('%d:0.000,0.000,0.00,0.000,0.0,0.0,%.1f,0.0,0.0,0.0,0.0,0.0'%(f,r) for f,r in keys))
def lines():
    R=SNAKE_RAISE
    return [MARK,
            'body 4cd0aad5 snake',
            'fit Spanking 4cd0aad5 scale=1.00 size=1.000   # em23 (snake): the game\'s placement, Gene\'s left arm raised off the back',
            gene(1337,[(0,0.0),(8,R),(22,R)]), gene(1339,[(0,R),(16,R)]), gene(1341,[(0,R),(12,R),(21,0.0)]),
            'fit Stinger 4cd0aad5 off   # em23 (snake): his own punish (K)',
            'fit CobraTwist 4cd0aad5 off   # em23 (snake): his own punish (K)']
if __name__=='__main__':
    L=open(FILE).read().rstrip('\n').split('\n')
    if MARK in L: L=L[:L.index(MARK)]
    open(FILE,'w').write('\n'.join(L+lines())+'\n')
