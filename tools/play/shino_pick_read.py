"""pick_read.py <ram dump>: see pick.sh.

The day's offers are the static table at 0x6271dc (0xa4 a mission, the offers first in the list's order): +0 the
reward, -0x10 1 while offered (2 for a marked one, the story's), +0x1c the client and +0x20 the target (lords: 0 Ichijo,
1 Akame, 2 Sadame; 14 and others are not a lord). The titles are UTF-16 after "News" in the list's text.

Prints k (1-based; 0: none), the mode for PICK_MODE=auto, the offers and the titles. Without LORD: a marked mission,
else a lord's own death, else the best paid. With LORD=<n> (serve one lord): never a mission against that lord (nor
one without a title); then a marked one the drivers can finish (mode()), one about a soul fragment, any they can
finish, that lord's, the best paid; 0 (no mission) when none is left that they can finish.
"""
import os
import re
import struct
import sys

ram = open(sys.argv[1], 'rb').read()
offered = []
for i in range(40):
    a = 0x6271dc + 0xa4 * i
    flag = struct.unpack_from('<i', ram, a - 0x10)[0]
    if flag not in (1, 2):
        break
    reward, client, target = (struct.unpack_from('<i', ram, a + o)[0] for o in (0, 0x1c, 0x20))
    offered.append((reward, client, target, flag))
if not offered:
    print(0)
    sys.exit()
titles = []
news = 'News'.encode('utf-16-le') + b'\0\0'
for m in re.finditer(re.escape(news), ram):
    if m.start() % 2:
        continue
    seg = ram[m.start():m.start() + 0x400]
    found = [x.decode('utf-16-le', 'ignore') for x in re.findall(rb'(?:[\x20-\x7e]\x00){4,}', seg)][1:]
    if found and '$' not in ''.join(found) and not {'Assassination', 'Duel', 'Guard'} & set(found):  # not the types
        merged = []  # a long title wraps: its second line starts in lowercase
        for x in found:
            if not re.match(r'[A-Za-z"\']', x):
                break  # "::>6": the end of the list's text
            if merged and x[:1].islower():
                merged[-1] = merged[-1].rstrip() + ' ' + x
            else:
                merged.append(x)
        titles = merged[:len(offered)]
titles += [''] * (len(offered) - len(titles))

def mode(title):
    """The way to play a mission, from its title (PICK_MODE=auto): None for one the drivers cannot finish."""
    t = title.lower()
    if re.search(r'mosu|usuba|ageha|kenobi|hebitonbo|taraba|kabuto|zaji|challenge', t):
        return 'mixed'  # a rival ninja (the Mosu have the third soul fragment): the kill loop
    if re.search(r'fragment|glowing stone|hidden stone|stone that|the jewel', t):
        return 'thief'  # a soul fragment: an item to take and carry out ("The stone that Akame is hiding")
    if re.search(r'collect|can you eat|protect|guard|rescue|escort|kidnap|bring .* alive', t):
        return None  # collection: the cheat completes it with nothing to send ("Embezzler"); guard: the ward dies
    if re.search(r'\bkill\b|defeat|assassinate|execute|rub out|eliminate|\bhead\b|destroy|attack|wipe|slay|barbarian|misery|'
                 r'in secret|annihilate', t):
        return 'mixed'  # the kill loop: the target really dies
    if re.search(r'steal|get back|get those|get the|take back|retrieve|recover|hidden fortune|borrow', t):
        return 'thief'
    if re.search(r'deliver|carry|transport', t):
        return 'goal'
    return 'done'  # reconnaissance and the rest: the completion cheat


lord = os.environ.get('LORD')
if lord is None:
    kills = [i for i, t in enumerate(titles)
             if re.match(r"(Kill|Defeat|Assassinate|Execute) (Nobuteru |Hideyoshi )?(Ichijo|Sadame|Akame)$", t)]
    marked = [i for i, o in enumerate(offered) if o[3] == 2]
    k = marked[0] if marked else kills[0] if kills else max(range(len(offered)), key=lambda i: offered[i][0])
else:
    lord = int(lord)

    def score(i):
        reward, client, target, flag = offered[i]
        # a soul fragment's mission: "Find the glowing stone", "A glowing stone in $MAP", "Steal (back) the jewel",
        # "Ichijo's glowing stone", "Akame's hidden stone", "Find a soul fragment", or a rival ninja's challenge ("A
        # challenge from Usuba": the Mosu have one); taken even against the lord (unseen, he does not find out). ("A
        # chance to dispose of the Mosu ninja" kills Mosu but gives no fragment.)
        fragment = bool(re.search(r'(?i)fragment|soul|glowing stone|hidden stone|stone that|the jewel|usuba|ageha|'
                                  r'hebitonbo|kabuto|zaji|challenge', titles[i]))
        if not titles[i]:
            # below the list's first lines (only the visible titles are in memory): the lord's best paid, against
            # another lord, is most likely a lord's or a samurai general's death ("Kill Sadame": 2,578 Mon)
            if any(titles) and client == lord and target in (0, 1, 2) and target != lord and reward >= 1500:
                return (False, False, True, True, reward)
            return None
        if target == lord and not fragment:
            return None
        doable = mode(titles[i]) is not None
        return (fragment, doable and flag == 2, doable, client == lord, reward)
    scores = [(score(i), i) for i in range(len(offered)) if score(i) is not None]
    k = max(scores)[1] if scores and (max(scores)[0][0] or max(scores)[0][2]) else -1
title = titles[k] if k >= 0 else ''
how = (mode(title) if title else 'mixed') if k >= 0 else 'done'
print(k + 1, how or 'done', *[f'{r}:{c}>{t}:{f}' for r, c, t, f in offered], '|',
      ' / '.join(titles).encode('ascii', 'replace').decode())
