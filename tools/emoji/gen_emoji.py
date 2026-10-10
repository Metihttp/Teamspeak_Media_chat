# 2.2 emoji: generates src/emojitable.inc, the emoji table of TS Media chat. Offline only: names come
# from Python's unicodedata, the rest is curated in this file (groups, order, names of sequences, search
# words). Nothing is downloaded.
#
# Two steps (run Python isolated, from the repository root):
#   python -I tools/emoji/gen_emoji.py candidates build-wt/emoji_candidates.txt
#   build-wt/Release/emoji_probe.exe build-wt/emoji_candidates.txt build-wt/emoji_probe.txt
#   python -I tools/emoji/gen_emoji.py table build-wt/emoji_probe.txt src/emojitable.inc
# The probe (tools/emoji_probe.cpp) asks DirectWrite which candidates Segoe UI Emoji draws as one colour
# picture on this PC; only those go into the table. Run it on the newest Windows available: older ones
# hide what their font lacks at run time (emoji::supported). "table --unfiltered" writes every candidate
# (only to bootstrap the probe itself).
#
# Writing the lists below: emoji separated by spaces, '+' joins the parts of a ZWJ sequence, variation
# selectors are added by rule (fully qualified forms), "kc:#" is a keycap, "ri:US" a regional-indicator
# flag, "tag:gbeng" a subdivision flag. "emoji|name" overrides the name.

import sys
import unicodedata

ZWJ = 0x200D
VS16 = 0xFE0F
VS15 = 0xFE0E
KEYCAP = 0x20E3
TONES = [0x1F3FB, 0x1F3FC, 0x1F3FD, 0x1F3FE, 0x1F3FF]
TONE_NAMES = ["light skin tone", "medium-light skin tone", "medium skin tone", "medium-dark skin tone", "dark skin tone"]

GROUPS = ["Smileys & people", "Animals & nature", "Food & drink", "Activities", "Travel & places", "Objects", "Symbols", "Flags"]

# Emoji_Presentation=Yes below U+1F000 (everything else there shows as text without U+FE0F).
EP_YES_LOW = set([0x231A, 0x231B, 0x23E9, 0x23EA, 0x23EB, 0x23EC, 0x23F0, 0x23F3, 0x25FD, 0x25FE, 0x2614, 0x2615]
                 + list(range(0x2648, 0x2654)) + [0x267F, 0x2693, 0x26A1, 0x26AA, 0x26AB, 0x26BD, 0x26BE, 0x26C4, 0x26C5, 0x26CE,
                 0x26D4, 0x26EA, 0x26F2, 0x26F3, 0x26F5, 0x26FA, 0x26FD, 0x2705, 0x270A, 0x270B, 0x2728, 0x274C, 0x274E, 0x2753,
                 0x2754, 0x2755, 0x2757, 0x2795, 0x2796, 0x2797, 0x27B0, 0x27BF, 0x2B1B, 0x2B1C, 0x2B50, 0x2B55])
# Emoji_Presentation=No from U+1F000 on.
EP_NO_HIGH = set([0x1F170, 0x1F171, 0x1F17E, 0x1F17F, 0x1F202, 0x1F237, 0x1F321, 0x1F324, 0x1F325, 0x1F326, 0x1F327, 0x1F328,
                  0x1F329, 0x1F32A, 0x1F32B, 0x1F32C, 0x1F336, 0x1F37D, 0x1F396, 0x1F397, 0x1F399, 0x1F39A, 0x1F39B, 0x1F39E,
                  0x1F39F, 0x1F3CB, 0x1F3CC, 0x1F3CD, 0x1F3CE, 0x1F3D4, 0x1F3D5, 0x1F3D6, 0x1F3D7, 0x1F3D8, 0x1F3D9, 0x1F3DA,
                  0x1F3DB, 0x1F3DC, 0x1F3DD, 0x1F3DE, 0x1F3DF, 0x1F3F3, 0x1F3F5, 0x1F3F7, 0x1F43F, 0x1F441, 0x1F4FD, 0x1F549,
                  0x1F54A, 0x1F56F, 0x1F570, 0x1F573, 0x1F574, 0x1F575, 0x1F576, 0x1F577, 0x1F578, 0x1F579, 0x1F587, 0x1F58A,
                  0x1F58B, 0x1F58C, 0x1F58D, 0x1F590, 0x1F5A5, 0x1F5A8, 0x1F5B1, 0x1F5B2, 0x1F5BC, 0x1F5C2, 0x1F5C3, 0x1F5C4,
                  0x1F5D1, 0x1F5D2, 0x1F5D3, 0x1F5DC, 0x1F5DD, 0x1F5DE, 0x1F5E1, 0x1F5E3, 0x1F5E8, 0x1F5EF, 0x1F5F3, 0x1F5FA,
                  0x1F6CB, 0x1F6CD, 0x1F6CE, 0x1F6CF, 0x1F6E0, 0x1F6E1, 0x1F6E2, 0x1F6E3, 0x1F6E4, 0x1F6E5, 0x1F6E9, 0x1F6F0,
                  0x1F6F3])

# Emoji_Modifier_Base: the first code point of everything that can take a skin tone (the probe drops
# combinations Segoe UI Emoji doesn't have).
MODIFIER_BASES = set(ord(c) for c in
    "☝⛹✊✋✌✍🎅🏂🏃🏄🏇🏊🏋🏌👂👃👆👇👈👉👊👋👌👍👎👏👐👦👧👨👩👫👬👭👮👰👱👲👳👴👵👶👷👸👼💁💂💃💅💆💇💏💑💪🕴🕵🕺🖐🖕🖖"
    "🙅🙆🙇🙋🙌🙍🙎🙏🚣🚴🚵🚶🛀🛌🤌🤏🤘🤙🤚🤛🤜🤝🤞🤟🤦🤰🤱🤲🤳🤴🤵🤶🤷🤸🤹🤽🤾🥷🦵🦶🦸🦹🦻🧍🧎🧏🧑🧒🧓🧔🧕🧖🧗🧘🧙🧚🧛🧜🧝"
    "🫃🫄🫅🫰🫱🫲🫳🫴🫵🫶🫷🫸")
PERSONS = set(ord(c) for c in "👨👩🧑👦👧🧒")

# ---- the groups, in picker order ----------------------------------------------------------------------

PROFESSIONS = [("⚕", "health worker"), ("🎓", "student"), ("🏫", "teacher"), ("⚖", "judge"), ("🌾", "farmer"), ("🍳", "cook"),
               ("🔧", "mechanic"), ("🏭", "factory worker"), ("💼", "office worker"), ("🔬", "scientist"), ("💻", "technologist"),
               ("🎤", "singer"), ("🎨", "artist"), ("✈", "pilot"), ("🚀", "astronaut"), ("🚒", "firefighter")]

# base, its name, the man / woman forms' names (None: derived from the name)
GENDERED = [
    ("🙍", "person frowning", None), ("🙎", "person pouting", None), ("🙅", "person gesturing NO", None), ("🙆", "person gesturing OK", None),
    ("💁", "person tipping hand", None), ("🙋", "person raising hand", None), ("🧏", "deaf person", ("deaf man", "deaf woman")),
    ("🙇", "person bowing", None), ("🤦", "person facepalming", None), ("🤷", "person shrugging", None),
]
GENDERED_ROLES = [
    ("👮", "police officer", None), ("🕵", "detective", None), ("💂", "guard", None), ("👷", "construction worker", None),
    ("👳", "person wearing turban", None), ("🤵", "person in tuxedo", None), ("👰", "person with veil", None),
]
GENDERED_FANTASY = [
    ("🦸", "superhero", None), ("🦹", "supervillain", None), ("🧙", "mage", None), ("🧚", "fairy", None), ("🧛", "vampire", None),
    ("🧜", "merperson", ("merman", "mermaid")), ("🧝", "elf", None), ("🧞", "genie", None), ("🧟", "zombie", None),
]
GENDERED_ACTIVITY = [
    ("💆", "person getting massage", None), ("💇", "person getting haircut", None), ("🚶", "person walking", None), ("🧍", "person standing", None),
    ("🧎", "person kneeling", None),
]
GENDERED_ACTIVITY2 = [
    ("🏃", "person running", None),
]
GENDERED_ACTIVITY3 = [
    ("👯", "people with bunny ears", ("men with bunny ears", "women with bunny ears")), ("🧖", "person in steamy room", None),
    ("🧗", "person climbing", None),
]
GENDERED_SPORT = [
    ("🏌", "person golfing", None), ("🏄", "person surfing", None), ("🚣", "person rowing boat", None), ("🏊", "person swimming", None),
    ("⛹", "person bouncing ball", None), ("🏋", "person lifting weights", None), ("🚴", "person biking", None), ("🚵", "person mountain biking", None),
    ("🤸", "person cartwheeling", None), ("🤼", "people wrestling", ("men wrestling", "women wrestling")), ("🤽", "person playing water polo", None),
    ("🤾", "person playing handball", None), ("🤹", "person juggling", None),
]
GENDERED_REST = [("🧘", "person in lotus position", None)]


def gendered(items):
    out = []
    for base, name, forms in items:
        if forms:
            man, woman = forms
        elif name.startswith("person "):
            man, woman = "man " + name[7:], "woman " + name[7:]
        elif name.startswith("people "):
            man, woman = "men " + name[7:], "women " + name[7:]
        else:
            man, woman = "man " + name, "woman " + name
        out += ["%s|%s" % (base, name), "%s+♂|%s" % (base, man), "%s+♀|%s" % (base, woman)]
    return " ".join(out)


def professions():
    out = []
    for part, role in PROFESSIONS:
        out += ["🧑+%s|%s" % (part, role), "👨+%s|man %s" % (part, role), "👩+%s|woman %s" % (part, role)]
    return " ".join(out)


SMILEYS = """
😀 😃|grinning face with big eyes 😄|grinning face with smiling eyes 😁|beaming face with smiling eyes 😆|grinning squinting face
😅|grinning face with sweat 🤣 😂 🙂 🙃 🫠 😉 😊 😇
🥰 😍|smiling face with heart-eyes 🤩 😘 😗 ☺|smiling face 😚 😙 🥲
😋 😛 😜|winking face with tongue 🤪 😝|squinting face with tongue 🤑
🤗|smiling face with open hands 🤭 🫢 🫣 🤫 🤔 🫡
🤐 🤨 😐 😑 😶 🫥 😶+🌫|face in clouds 😏 😒 🙄 😬 😮+💨|face exhaling 🤥 🫨 🙂+↔|head shaking horizontally 🙂+↕|head shaking vertically
😌 😔 😪 🤤 😴 🫩
😷 🤒 🤕 🤢 🤮 🤧 🥵 🥶 🥴 😵|face with crossed-out eyes 😵+💫|face with spiral eyes 🤯
🤠 🥳 🥸 😎 🤓 🧐
😕 🫤 😟 🙁 ☹|frowning face 😮 😯 😲 😳 🥺 🥹 😦 😧 😨 😰|anxious face with sweat 😥 😢 😭 😱 😖 😣 😞 😓 😩 😫 🥱
😤|face with steam from nose 😡|enraged face 😠 🤬 😈 👿 💀 ☠
💩|pile of poo 🤡 👹 👺 👻 👽 👾 🤖
😺|grinning cat 😸|grinning cat with smiling eyes 😹 😻 😼 😽 🙀 😿 😾 🙈 🙉 🙊
💌 💘 💝 💖 💗 💓 💞 💕 💟 ❣ 💔 ❤+🔥|heart on fire ❤+🩹|mending heart ❤|red heart 🩷 🧡 💛 💚 💙 🩵 💜 🤎 🖤 🩶 🤍
💋|kiss mark 💯 💢 💥|collision 💫 💦 💨 🕳 💬 👁+🗨|eye in speech bubble 🗨 🗯 💭 💤
👋 🤚 🖐 ✋ 🖖 🫱 🫲 🫳 🫴 🫷 🫸
👌 🤌 🤏 ✌|victory hand 🤞 🫰 🤟 🤘 🤙
👈 👉 👆 🖕 👇 ☝ 🫵
👍|thumbs up 👎|thumbs down ✊ 👊|oncoming fist 🤛 🤜
👏 🙌 🫶 👐 🤲 🤝 🙏|folded hands
✍ 💅 🤳 💪 🦾 🦿 🦵 🦶 👂 🦻 👃 🧠 🫀 🫁 🦷 🦴 👀 👁 👅 👄 🫦
👶 🧒 👦 👧 🧑 👱|person: blond hair 👨 🧔|person: beard 🧔+♂|man: beard 🧔+♀|woman: beard
👨+🦰|man: red hair 👨+🦱|man: curly hair 👨+🦳|man: white hair 👨+🦲|man: bald
👩 👩+🦰|woman: red hair 🧑+🦰|person: red hair 👩+🦱|woman: curly hair 🧑+🦱|person: curly hair
👩+🦳|woman: white hair 🧑+🦳|person: white hair 👩+🦲|woman: bald 🧑+🦲|person: bald
👱+♀|woman: blond hair 👱+♂|man: blond hair 🧓 👴 👵
""" + gendered(GENDERED) + " " + professions() + " " + gendered(GENDERED_ROLES) + """
🥷 🫅 🤴 👸 👲|person with skullcap 🧕 🤰 🫃 🫄 🤱 👩+🍼|woman feeding baby 👨+🍼|man feeding baby 🧑+🍼|person feeding baby
👼 🎅 🤶 🧑+🎄|mx claus
""" + gendered(GENDERED_FANTASY) + " 🧌 " + gendered(GENDERED_ACTIVITY) + """
🧑+🦯|person with white cane 👨+🦯|man with white cane 👩+🦯|woman with white cane
🧑+🦼|person in motorized wheelchair 👨+🦼|man in motorized wheelchair 👩+🦼|woman in motorized wheelchair
🧑+🦽|person in manual wheelchair 👨+🦽|man in manual wheelchair 👩+🦽|woman in manual wheelchair
""" + gendered(GENDERED_ACTIVITY2) + " 💃 🕺 🕴|person in suit levitating " + gendered(GENDERED_ACTIVITY3) + """
🤺 🏇 ⛷ 🏂
""" + gendered(GENDERED_SPORT) + " " + gendered(GENDERED_REST) + """
🛀 🛌|person in bed
🧑+🤝+🧑|people holding hands 👭 👫 👬 💏 👩+❤+💋+👨|kiss: woman, man 👨+❤+💋+👨|kiss: man, man 👩+❤+💋+👩|kiss: woman, woman
💑 👩+❤+👨|couple with heart: woman, man 👨+❤+👨|couple with heart: man, man 👩+❤+👩|couple with heart: woman, woman
👪 👨+👩+👦|family: man, woman, boy 👨+👩+👧|family: man, woman, girl 👨+👩+👧+👦|family: man, woman, girl, boy
👨+👩+👦+👦|family: man, woman, boy, boy 👨+👩+👧+👧|family: man, woman, girl, girl 👨+👨+👦|family: man, man, boy
👨+👨+👧|family: man, man, girl 👨+👨+👧+👦|family: man, man, girl, boy 👨+👨+👦+👦|family: man, man, boy, boy
👨+👨+👧+👧|family: man, man, girl, girl 👩+👩+👦|family: woman, woman, boy 👩+👩+👧|family: woman, woman, girl
👩+👩+👧+👦|family: woman, woman, girl, boy 👩+👩+👦+👦|family: woman, woman, boy, boy 👩+👩+👧+👧|family: woman, woman, girl, girl
👨+👦|family: man, boy 👨+👦+👦|family: man, boy, boy 👨+👧|family: man, girl 👨+👧+👦|family: man, girl, boy 👨+👧+👧|family: man, girl, girl
👩+👦|family: woman, boy 👩+👦+👦|family: woman, boy, boy 👩+👧|family: woman, girl 👩+👧+👦|family: woman, girl, boy 👩+👧+👧|family: woman, girl, girl
🧑+🧑+🧒|family: adult, adult, child 🧑+🧑+🧒+🧒|family: adult, adult, child, child 🧑+🧒|family: adult, child 🧑+🧒+🧒|family: adult, child, child
🗣 👤 👥 🫂 👣 🫆
"""

ANIMALS = """
🐵 🐒 🦍 🦧 🐶|dog face 🐕 🦮 🐕+🦺|service dog 🐩 🐺 🦊 🦝 🐱|cat face 🐈 🐈+⬛|black cat 🦁 🐯|tiger face 🐅 🐆 🐴|horse face 🫎 🫏 🐎 🦄 🦓 🦌 🦬
🐮|cow face 🐂 🐃 🐄 🐷|pig face 🐖 🐗 🐽 🐏 🐑 🐐 🐪 🐫 🦙 🦒 🐘 🦣 🦏 🦛 🐭|mouse face 🐁 🐀 🐹 🐰|rabbit face 🐇 🐿 🦫 🦔 🦇
🐻 🐻+❄|polar bear 🐨 🐼 🦥 🦦 🦨 🦘 🦡 🐾
🦃 🐔 🐓 🐣 🐤 🐥 🐦 🐧 🕊 🦅 🦆 🦢 🦉 🦤 🪶 🦩 🦚 🦜 🪽 🐦+⬛|black bird 🪿 🐦+🔥|phoenix
🐸|frog 🐊 🐢 🦎 🐍 🐲 🐉 🦕 🦖
🐳 🐋 🐬 🦭 🐟 🐠 🐡 🦈 🐙 🐚 🪸 🪼
🐌 🦋 🐛 🐜 🐝|honeybee 🪲 🐞 🦗 🪳 🕷 🕸 🦂 🦟 🪰 🪱 🦠
💐 🌸 💮 🪷 🏵 🌹 🥀 🌺 🌻 🌼 🌷 🪻
🌱 🪴 🌲 🌳 🌴 🌵 🌾 🌿 ☘ 🍀 🍁 🍂 🍃 🪹 🪺 🍄 🪾
"""

FOOD = """
🍇 🍈 🍉 🍊 🍋 🍋+🟩|lime 🍌 🍍 🥭 🍎 🍏 🍐 🍑 🍒 🍓 🫐 🥝 🍅 🫒 🥥
🥑 🍆 🥔 🥕 🌽 🌶 🫑 🥒 🥬 🥦 🧄 🧅 🥜 🫘 🌰 🫚 🫛 🍄+🟫|brown mushroom 🫜
🍞 🥐 🥖 🫓 🥨 🥯 🥞 🧇 🧀 🍖 🍗 🥩 🥓 🍔 🍟 🍕 🌭 🥪 🌮 🌯 🫔 🥙 🧆 🥚 🍳 🥘 🍲 🫕 🥣 🥗 🍿 🧈 🧂 🥫
🍱 🍘 🍙 🍚 🍛 🍜 🍝 🍠 🍢 🍣 🍤 🍥 🥮 🍡 🥟 🥠 🥡
🦀 🦞 🦐 🦑 🦪
🍦 🍧 🍨 🍩 🍪 🎂 🍰 🧁 🥧 🍫 🍬 🍭 🍮 🍯
🍼 🥛 ☕ 🫖 🍵 🍶 🍾 🍷 🍸 🍹 🍺 🍻 🥂 🥃 🫗 🥤 🧋 🧃 🧉 🧊
🥢 🍽 🍴 🥄 🔪 🫙 🏺
"""

TRAVEL = """
🌍 🌎 🌏 🌐 🗺 🗾 🧭
🏔 ⛰ 🌋 🗻 🏕 🏖 🏜 🏝 🏞
🏟 🏛 🏗 🧱 🪨 🪵 🛖 🏘 🏚 🏠 🏡 🏢 🏣 🏤 🏥 🏦 🏨 🏩 🏪 🏫 🏬 🏭 🏯 🏰 💒 🗼 🗽
⛪ 🕌 🛕 🕍 ⛩ 🕋
⛲ ⛺ 🌁 🌃 🏙 🌄 🌅 🌆 🌇 🌉 ♨ 🎠 🛝 🎡 🎢 💈 🎪
🚂 🚃 🚄 🚅 🚆 🚇 🚈 🚉 🚊 🚝 🚞 🚋 🚌 🚍 🚎 🚐 🚑 🚒 🚓 🚔 🚕 🚖 🚗 🚘 🚙 🛻 🚚 🚛 🚜 🏎 🏍 🛵 🦽 🦼 🛺 🚲 🛴 🛹 🛼
🚏 🛣 🛤 🛢 ⛽ 🛞 🚨 🚥 🚦 🛑 🚧
⚓ 🛟 ⛵ 🛶 🚤 🛳 ⛴ 🛥 🚢
✈ 🛩 🛫 🛬 🪂 💺 🚁 🚟 🚠 🚡 🛰 🚀 🛸
🛎 🧳
⌛ ⏳ ⌚ ⏰ ⏱ ⏲ 🕰 🕛 🕧 🕐 🕜 🕑 🕝 🕒 🕞 🕓 🕟 🕔 🕠 🕕 🕡 🕖 🕢 🕗 🕣 🕘 🕤 🕙 🕥 🕚 🕦
🌑 🌒 🌓 🌔 🌕 🌖 🌗 🌘 🌙 🌚 🌛 🌜 🌡 ☀ 🌝 🌞 🪐 ⭐ 🌟 🌠 🌌
☁ ⛅ ⛈ 🌤 🌥 🌦 🌧 🌨 🌩 🌪 🌫 🌬 🌀 🌈 🌂 ☂ ☔ ⛱ ⚡ ❄ ☃ ⛄ ☄ 🔥 💧 🌊
"""

ACTIVITIES = """
🎃 🎄 🎆 🎇 🧨 ✨ 🎈 🎉 🎊 🎋 🎍 🎎 🎏 🎐 🎑 🧧 🎀 🎁 🎗 🎟 🎫
🎖 🏆 🏅 🥇 🥈 🥉
⚽ ⚾ 🥎 🏀 🏐 🏈 🏉 🎾 🥏 🎳 🏏 🏑 🏒 🥍 🏓 🏸 🥊 🥋 🥅 ⛳ ⛸ 🎣 🤿 🎽 🎿 🛷 🥌
🎯 🪀 🪁 🔫|water pistol 🎱 🔮 🪄 🎮 🕹 🎰 🎲 🧩 🧸 🪅 🪩 🪆
♠ ♥ ♦ ♣ ♟ 🃏 🀄 🎴
🎭 🖼 🎨 🧵 🪡 🧶 🪢
"""

OBJECTS = """
👓 🕶 🥽 🥼 🦺 👔 👕 👖 🧣 🧤 🧥 🧦 👗 👘 🥻 🩱 🩲 🩳 👙 👚 🪭 👛 👜 👝 🛍 🎒 🩴 👞 👟 🥾 🥿 👠 👡 🩰 👢 🪮
👑 👒 🎩 🎓 🧢 🪖 ⛑ 📿 💄 💍 💎
🔇 🔈 🔉 🔊 📢 📣 📯 🔔 🔕
🎼 🎵 🎶 🎙 🎚 🎛 🎤 🎧 📻
🎷 🪗 🎸 🎹 🎺 🎻 🪕 🥁 🪘 🪇 🪈 🪉
📱 📲 ☎ 📞 📟 📠
🔋 🪫 🔌 💻 🖥 🖨 ⌨ 🖱 🖲 💽 💾 💿 📀 🧮
🎥 🎞 📽 🎬 📺 📷 📸 📹 📼 🔍 🔎 🕯 💡 🔦 🏮 🪔
📔 📕 📖 📗 📘 📙 📚 📓 📒 📃 📜 📄 📰 🗞 📑 🔖 🏷
💰 🪙 💴 💵 💶 💷 💸 💳 🧾 💹
✉ 📧 📨 📩 📤 📥 📦 📫 📪 📬 📭 📮 🗳
✏ ✒ 🖋 🖊 🖌 🖍 📝
💼 📁 📂 🗂 📅 📆 🗒 🗓 📇 📈 📉 📊 📋 📌 📍 📎 🖇 📏 📐 ✂ 🗃 🗄 🗑
🔒 🔓 🔏 🔐 🔑 🗝
🔨 🪓 ⛏ ⚒ 🛠 🗡 ⚔ 💣 🪃 🏹 🛡 🪚 🔧 🪛 🔩 ⚙ 🗜 ⚖ 🦯 🔗 ⛓+💥|broken chain ⛓ 🪝 🧰 🧲 🪜 🪏
⚗ 🧪 🧫 🧬 🔬 🔭 📡
💉 🩸 💊 🩹 🩼 🩺 🩻
🚪 🛗 🪞 🪟 🛏 🛋 🪑 🚽 🪠 🚿 🛁 🪤 🪒 🧴 🧷 🧹 🧺 🧻 🪣 🧼 🫧 🪥 🧽 🧯 🛒
🚬 ⚰ 🪦 ⚱ 🧿 🪬 🗿 🪧 🪪
"""

SYMBOLS = """
🏧 🚮 🚰 ♿ 🚹 🚺 🚻 🚼 🚾 🛂 🛃 🛄 🛅
⚠ 🚸 ⛔ 🚫 🚳 🚭 🚯 🚱 🚷 📵 🔞 ☢ ☣
⬆ ↗ ➡ ↘ ⬇ ↙ ⬅ ↖ ↕ ↔ ↩ ↪ ⤴ ⤵ 🔃 🔄 🔙 🔚 🔛 🔜 🔝
🛐 ⚛ 🕉 ✡ ☸ ☯ ✝ ☦ ☪ ☮ 🕎 🔯 🪯
♈ ♉ ♊ ♋ ♌ ♍ ♎ ♏ ♐ ♑ ♒ ♓ ⛎
🔀 🔁 🔂 ▶ ⏩ ⏭ ⏯ ◀ ⏪ ⏮ 🔼 ⏫ 🔽 ⏬ ⏸ ⏹ ⏺ ⏏ 🎦 🔅 🔆 📶 🛜 📳 📴
♀ ♂ ⚧
✖ ➕ ➖ ➗ 🟰 ♾
‼ ⁉ ❓ ❔ ❕ ❗ 〰
💱 💲
⚕ ♻ ⚜ 🔱 📛 🔰 ⭕ ✅ ☑ ✔ ❌ ❎ ➰ ➿ 〽 ✳ ✴ ❇ © ® ™ 🫟
kc:# kc:* kc:0 kc:1 kc:2 kc:3 kc:4 kc:5 kc:6 kc:7 kc:8 kc:9 🔟|keycap: 10
🔠 🔡 🔢 🔣 🔤 🅰 🆎 🅱 🆑 🆒 🆓 ℹ 🆔 Ⓜ 🆕 🆖 🅾 🆗 🅿 🆘 🆙 🆚
🈁 🈂 🈷 🈶 🈯 🉐 🈹 🈚 🈲 🉑 🈸 🈴 🈳 ㊗ ㊙ 🈺 🈵
🔴 🟠 🟡 🟢 🔵 🟣 🟤 ⚫ ⚪ 🟥 🟧 🟨 🟩 🟦 🟪 🟫 ⬛ ⬜ ◼ ◻ ◾ ◽ ▪ ▫ 🔶 🔷 🔸 🔹 🔺 🔻 💠 🔘 🔳 🔲
"""

REGIONS = ("AC AD AE AF AG AI AL AM AO AQ AR AS AT AU AW AX AZ BA BB BD BE BF BG BH BI BJ BL BM BN BO BQ BR BS BT BV BW BY BZ "
           "CA CC CD CF CG CH CI CK CL CM CN CO CP CQ CR CU CV CW CX CY CZ DE DG DJ DK DM DO DZ EA EC EE EG EH ER ES ET EU FI FJ FK "
           "FM FO FR GA GB GD GE GF GG GH GI GL GM GN GP GQ GR GS GT GU GW GY HK HM HN HR HT HU IC ID IE IL IM IN IO IQ IR IS IT JE "
           "JM JO JP KE KG KH KI KM KN KP KR KW KY KZ LA LB LC LI LK LR LS LT LU LV LY MA MC MD ME MF MG MH MK ML MM MN MO MP MQ MR "
           "MS MT MU MV MW MX MY MZ NA NC NE NF NG NI NL NO NP NR NU NZ OM PA PE PF PG PH PK PL PM PN PR PS PT PW PY QA RE RO RS RU "
           "RW SA SB SC SD SE SG SH SI SJ SK SL SM SN SO SR SS ST SV SX SY SZ TA TC TD TF TG TH TJ TK TL TM TN TO TR TT TV TW TZ UA "
           "UG UM UN US UY UZ VA VC VE VG VI VN VU WF WS XK YE YT ZA ZM ZW")

FLAGS = ("🏁 🚩 🎌 🏴 🏳|white flag 🏳+🌈|rainbow flag 🏳+⚧|transgender flag 🏴+☠|pirate flag "
         + " ".join("ri:" + r for r in REGIONS.split())
         + " tag:gbeng|flag: England tag:gbsct|flag: Scotland tag:gbwls|flag: Wales")

LISTS = [SMILEYS, ANIMALS, FOOD, ACTIVITIES, TRAVEL, OBJECTS, SYMBOLS, FLAGS]
# The group order is the picker's; the lists above are written in Unicode's order, with Activities moved
# before Travel the way chat apps show them.
LIST_GROUP = [0, 1, 2, 3, 4, 5, 6, 7]

# Better names than the formal character names (CLDR-style short names).
NAMES = {
    "🙂": "slightly smiling face", "😉": "winking face", "😊": "smiling face with smiling eyes", "😇": "smiling face with halo",
    "🥰": "smiling face with hearts", "🤩": "star-struck", "😘": "face blowing a kiss", "😚": "kissing face with closed eyes",
    "😙": "kissing face with smiling eyes", "🥲": "smiling face with tear", "😋": "face savoring food", "🤪": "zany face",
    "🤑": "money-mouth face", "🤭": "face with hand over mouth", "🤫": "shushing face", "🤔": "thinking face", "🤐": "zipper-mouth face",
    "🤨": "face with raised eyebrow", "😐": "neutral face", "😶": "face without mouth", "😏": "smirking face", "😒": "unamused face",
    "🙄": "face with rolling eyes", "😬": "grimacing face", "🤥": "lying face", "😌": "relieved face", "😔": "pensive face",
    "😪": "sleepy face", "🤤": "drooling face", "😴": "sleeping face", "😷": "face with medical mask", "🤒": "face with thermometer",
    "🤕": "face with head-bandage", "🤢": "nauseated face", "🤮": "face vomiting", "🤧": "sneezing face", "🥵": "hot face",
    "🥶": "cold face", "🥴": "woozy face", "🤯": "exploding head", "🤠": "cowboy hat face", "🥳": "partying face", "🥸": "disguised face",
    "😎": "smiling face with sunglasses", "🤓": "nerd face", "🧐": "face with monocle", "😕": "confused face", "😟": "worried face",
    "🙁": "slightly frowning face", "😮": "face with open mouth", "😯": "hushed face", "😲": "astonished face", "😳": "flushed face",
    "🥺": "pleading face", "😦": "frowning face with open mouth", "😧": "anguished face", "😨": "fearful face", "😥": "sad but relieved face",
    "😢": "crying face", "😭": "loudly crying face", "😱": "face screaming in fear", "😖": "confounded face", "😣": "persevering face",
    "😞": "disappointed face", "😓": "downcast face with sweat", "😩": "weary face", "😫": "tired face", "🥱": "yawning face",
    "😠": "angry face", "🤬": "face with symbols on mouth", "😈": "smiling face with horns", "👿": "angry face with horns", "☠": "skull and crossbones",
    "🤡": "clown face", "👹": "ogre", "👺": "goblin", "👻": "ghost", "👽": "alien", "👾": "alien monster", "🤖": "robot",
    "😹": "cat with tears of joy", "😻": "smiling cat with heart-eyes", "😼": "cat with wry smile", "😽": "kissing cat", "🙀": "weary cat",
    "😿": "crying cat", "😾": "pouting cat", "🙈": "see-no-evil monkey", "🙉": "hear-no-evil monkey", "🙊": "speak-no-evil monkey",
    "💌": "love letter", "💘": "heart with arrow", "💝": "heart with ribbon", "💖": "sparkling heart", "💗": "growing heart",
    "💓": "beating heart", "💞": "revolving hearts", "💕": "two hearts", "💟": "heart decoration", "❣": "heart exclamation",
    "💔": "broken heart", "💯": "hundred points", "💢": "anger symbol", "💫": "dizzy", "💦": "sweat droplets", "💨": "dashing away",
    "🕳": "hole", "💬": "speech balloon", "🗨": "left speech bubble", "🗯": "right anger bubble", "💭": "thought balloon", "💤": "ZZZ",
    "👋": "waving hand", "🤚": "raised back of hand", "🖐": "hand with fingers splayed", "✋": "raised hand", "🖖": "vulcan salute",
    "👌": "OK hand", "🤌": "pinched fingers", "🤏": "pinching hand", "🤞": "crossed fingers", "🤟": "love-you gesture",
    "🤘": "sign of the horns", "🤙": "call me hand", "👈": "backhand index pointing left", "👉": "backhand index pointing right",
    "👆": "backhand index pointing up", "🖕": "middle finger", "👇": "backhand index pointing down", "☝": "index pointing up",
    "🫵": "index pointing at the viewer", "✊": "raised fist", "🤛": "left-facing fist", "🤜": "right-facing fist", "👏": "clapping hands",
    "🙌": "raising hands", "🫶": "heart hands", "👐": "open hands", "🤲": "palms up together", "🤝": "handshake", "✍": "writing hand",
    "💅": "nail polish", "🤳": "selfie", "💪": "flexed biceps", "👀": "eyes", "👁": "eye", "👅": "tongue", "👄": "mouth",
    "👶": "baby", "🧒": "child", "👦": "boy", "👧": "girl", "🧑": "person", "👨": "man", "👩": "woman", "🧓": "older person",
    "👴": "old man", "👵": "old woman", "🥷": "ninja", "🫅": "person with crown", "🤴": "prince", "👸": "princess",
    "🧕": "woman with headscarf", "🤰": "pregnant woman", "🫃": "pregnant man", "🫄": "pregnant person", "🤱": "breast-feeding",
    "👼": "baby angel", "🎅": "Santa Claus", "🤶": "Mrs. Claus", "🧌": "troll", "💃": "woman dancing", "🕺": "man dancing",
    "🤺": "person fencing", "🏇": "horse racing", "⛷": "skier", "🏂": "snowboarder", "🛀": "person taking bath",
    "👭": "women holding hands", "👫": "woman and man holding hands", "👬": "men holding hands", "💏": "kiss", "💑": "couple with heart",
    "👪": "family", "🗣": "speaking head", "👤": "bust in silhouette", "👥": "busts in silhouette", "🫂": "people hugging",
    "👣": "footprints", "🐕": "dog", "🐈": "cat", "🐅": "tiger", "🐎": "horse", "🐂": "ox", "🐄": "cow", "🐖": "pig", "🐀": "rat",
    "🐁": "mouse", "🐇": "rabbit", "🐿": "chipmunk", "🐻": "bear", "🐾": "paw prints", "🐔": "chicken", "🐣": "hatching chick",
    "🐤": "baby chick", "🐥": "front-facing baby chick", "🕊": "dove", "🐲": "dragon face", "🐳": "spouting whale", "🐠": "tropical fish",
    "🐚": "spiral shell", "🐞": "lady beetle", "🕷": "spider", "🕸": "spider web", "🦠": "microbe", "💐": "bouquet", "💮": "white flower",
    "🏵": "rosette", "🥀": "wilted flower", "🌱": "seedling", "🪴": "potted plant", "🌲": "evergreen tree", "🌳": "deciduous tree",
    "🌾": "sheaf of rice", "🌿": "herb", "☘": "shamrock", "🍀": "four leaf clover", "🍁": "maple leaf", "🍂": "fallen leaf",
    "🍃": "leaf fluttering in wind", "🍄": "mushroom", "🍊": "tangerine", "🍎": "red apple", "🍏": "green apple", "🥝": "kiwi fruit",
    "🌽": "ear of corn", "🌶": "hot pepper", "🥬": "leafy green", "🥜": "peanuts", "🌰": "chestnut", "🍖": "meat on bone",
    "🍗": "poultry leg", "🥩": "cut of meat", "🍔": "hamburger", "🍟": "french fries", "🍕": "pizza", "🍳": "cooking",
    "🥘": "shallow pan of food", "🍲": "pot of food", "🥣": "bowl with spoon", "🥗": "green salad", "🍱": "bento box",
    "🍙": "rice ball", "🍚": "cooked rice", "🍛": "curry rice", "🍜": "steaming bowl", "🍝": "spaghetti", "🍠": "roasted sweet potato",
    "🍥": "fish cake with swirl", "🍦": "soft ice cream", "🍧": "shaved ice", "🍪": "cookie", "🎂": "birthday cake", "🍰": "shortcake",
    "🍼": "baby bottle", "🥛": "glass of milk", "☕": "hot beverage", "🍵": "teacup without handle", "🍾": "bottle with popping cork",
    "🍷": "wine glass", "🍸": "cocktail glass", "🍹": "tropical drink", "🍺": "beer mug", "🍻": "clinking beer mugs",
    "🥂": "clinking glasses", "🥃": "tumbler glass", "🥤": "cup with straw", "🧋": "bubble tea", "🧃": "beverage box", "🧊": "ice",
    "🍽": "fork and knife with plate", "🍴": "fork and knife", "🔪": "kitchen knife", "🏺": "amphora", "🌍": "globe showing Europe-Africa",
    "🌎": "globe showing Americas", "🌏": "globe showing Asia-Australia", "🌐": "globe with meridians", "🗺": "world map",
    "🗾": "map of Japan", "🏔": "snow-capped mountain", "🗻": "mount fuji", "🏕": "camping", "🏖": "beach with umbrella",
    "🏝": "desert island", "🏞": "national park", "🏛": "classical building", "🏗": "building construction", "🏘": "houses",
    "🏚": "derelict house", "🏢": "office building", "🏣": "Japanese post office", "🏤": "post office", "🏯": "Japanese castle",
    "🏰": "castle", "💒": "wedding", "🗼": "Tokyo tower", "🗽": "Statue of Liberty", "⛩": "shinto shrine", "🌁": "foggy",
    "🌃": "night with stars", "🏙": "cityscape", "🌄": "sunrise over mountains", "🌅": "sunrise", "🌆": "cityscape at dusk",
    "🌇": "sunset", "🌉": "bridge at night", "♨": "hot springs", "🎠": "carousel horse", "🎡": "ferris wheel", "🎢": "roller coaster",
    "💈": "barber pole", "🎪": "circus tent", "🚂": "locomotive", "🚃": "railway car", "🚄": "high-speed train", "🚅": "bullet train",
    "🚆": "train", "🚇": "metro", "🚉": "station", "🚝": "monorail", "🚋": "tram car", "🚍": "oncoming bus", "🚐": "minibus",
    "🚓": "police car", "🚔": "oncoming police car", "🚖": "oncoming taxi", "🚗": "automobile", "🚘": "oncoming automobile",
    "🚙": "sport utility vehicle", "🛻": "pickup truck", "🚚": "delivery truck", "🚛": "articulated lorry", "🏎": "racing car",
    "🏍": "motorcycle", "🛵": "motor scooter", "🛺": "auto rickshaw", "🚲": "bicycle", "🛴": "kick scooter", "🚏": "bus stop",
    "🛣": "motorway", "🛤": "railway track", "🛢": "oil drum", "⛽": "fuel pump", "🚨": "police car light", "🚥": "horizontal traffic light",
    "🚦": "vertical traffic light", "🛑": "stop sign", "🚧": "construction", "🛳": "passenger ship", "⛴": "ferry",
    "🛥": "motor boat", "✈": "airplane", "🛩": "small airplane", "🛫": "airplane departure", "🛬": "airplane arrival",
    "💺": "seat", "🚟": "suspension railway", "🛰": "satellite", "🛸": "flying saucer", "🛎": "bellhop bell", "⌛": "hourglass done",
    "⏳": "hourglass not done", "⏱": "stopwatch", "⏲": "timer clock", "🕰": "mantelpiece clock", "🌙": "crescent moon",
    "🌚": "new moon face", "🌛": "first quarter moon face", "🌜": "last quarter moon face", "🌡": "thermometer", "☀": "sun",
    "🌝": "full moon face", "🌞": "sun with face", "🪐": "ringed planet", "⭐": "star", "🌟": "glowing star", "🌠": "shooting star",
    "🌌": "milky way", "☁": "cloud", "⛅": "sun behind cloud", "⛈": "cloud with lightning and rain", "🌤": "sun behind small cloud",
    "🌥": "sun behind large cloud", "🌦": "sun behind rain cloud", "🌧": "cloud with rain", "🌨": "cloud with snow",
    "🌩": "cloud with lightning", "🌪": "tornado", "🌫": "fog", "🌬": "wind face", "🌀": "cyclone", "🌂": "closed umbrella",
    "☂": "umbrella", "☔": "umbrella with rain drops", "⛱": "umbrella on ground", "⚡": "high voltage", "❄": "snowflake",
    "☃": "snowman", "⛄": "snowman without snow", "☄": "comet", "🔥": "fire", "💧": "droplet", "🌊": "water wave",
    "🎃": "jack-o-lantern", "🎄": "Christmas tree", "🎆": "fireworks", "🎇": "sparkler", "✨": "sparkles", "🎈": "balloon",
    "🎉": "party popper", "🎊": "confetti ball", "🎋": "tanabata tree", "🎍": "pine decoration", "🎎": "Japanese dolls",
    "🎏": "carp streamer", "🎐": "wind chime", "🎑": "moon viewing ceremony", "🧧": "red envelope", "🎀": "ribbon",
    "🎁": "wrapped gift", "🎗": "reminder ribbon", "🎟": "admission tickets", "🎫": "ticket", "🎖": "military medal",
    "🏆": "trophy", "🏅": "sports medal", "🥇": "1st place medal", "🥈": "2nd place medal", "🥉": "3rd place medal",
    "⚽": "soccer ball", "🏈": "american football", "🏉": "rugby football", "🥏": "flying disc", "🏏": "cricket game",
    "🏑": "field hockey", "🏒": "ice hockey", "🏓": "ping pong", "⛳": "flag in hole", "⛸": "ice skate", "🎣": "fishing pole",
    "🎽": "running shirt", "🎿": "skis", "🎯": "bullseye", "🎱": "pool 8 ball", "🔮": "crystal ball", "🎮": "video game",
    "🕹": "joystick", "🎰": "slot machine", "🎲": "game die", "🧩": "puzzle piece", "🧸": "teddy bear", "🪩": "mirror ball",
    "♠": "spade suit", "♥": "heart suit", "♦": "diamond suit", "♣": "club suit", "♟": "chess pawn", "🃏": "joker",
    "🀄": "mahjong red dragon", "🎴": "flower playing cards", "🎭": "performing arts", "🖼": "framed picture", "🎨": "artist palette",
    "👓": "glasses", "🕶": "sunglasses", "🥼": "lab coat", "🦺": "safety vest", "👔": "necktie", "👕": "t-shirt", "👖": "jeans",
    "👗": "dress", "👙": "bikini", "👚": "woman’s clothes", "👛": "purse", "👜": "handbag", "👝": "clutch bag", "🛍": "shopping bags",
    "🎒": "backpack", "🩴": "thong sandal", "👞": "man’s shoe", "👟": "running shoe", "🥾": "hiking boot", "🥿": "flat shoe",
    "👠": "high-heeled shoe", "👡": "woman’s sandal", "👢": "woman’s boot", "👒": "woman’s hat", "🎩": "top hat",
    "🎓": "graduation cap", "🧢": "billed cap", "⛑": "rescue worker’s helmet", "📿": "prayer beads", "💄": "lipstick", "💍": "ring",
    "💎": "gem stone", "🔇": "muted speaker", "🔈": "speaker low volume", "🔉": "speaker medium volume", "🔊": "speaker high volume",
    "📢": "loudspeaker", "📣": "megaphone", "📯": "postal horn", "🔔": "bell", "🔕": "bell with slash", "🎼": "musical score",
    "🎵": "musical note", "🎶": "musical notes", "🎙": "studio microphone", "🎚": "level slider", "🎛": "control knobs",
    "🎤": "microphone", "🎧": "headphone", "📻": "radio", "🎷": "saxophone", "🎸": "guitar", "🎹": "musical keyboard",
    "🎺": "trumpet", "🎻": "violin", "🥁": "drum", "📱": "mobile phone", "📲": "mobile phone with arrow", "☎": "telephone",
    "📞": "telephone receiver", "📟": "pager", "📠": "fax machine", "🔋": "battery", "🪫": "low battery", "🔌": "electric plug",
    "💻": "laptop", "🖥": "desktop computer", "🖨": "printer", "⌨": "keyboard", "🖱": "computer mouse", "🖲": "trackball",
    "💽": "computer disk", "💾": "floppy disk", "💿": "optical disk", "📀": "dvd", "🧮": "abacus", "🎥": "movie camera",
    "🎞": "film frames", "📽": "film projector", "🎬": "clapper board", "📺": "television", "📷": "camera", "📸": "camera with flash",
    "📹": "video camera", "📼": "videocassette", "🔍": "magnifying glass tilted left", "🔎": "magnifying glass tilted right",
    "🕯": "candle", "💡": "light bulb", "🔦": "flashlight", "🏮": "red paper lantern", "🪔": "diya lamp",
    "📔": "notebook with decorative cover", "📕": "closed book", "📖": "open book", "📗": "green book", "📘": "blue book",
    "📙": "orange book", "📚": "books", "📓": "notebook", "📒": "ledger", "📃": "page with curl", "📜": "scroll",
    "📄": "page facing up", "📰": "newspaper", "🗞": "rolled-up newspaper", "📑": "bookmark tabs", "🔖": "bookmark", "🏷": "label",
    "💰": "money bag", "🪙": "coin", "💴": "yen banknote", "💵": "dollar banknote", "💶": "euro banknote", "💷": "pound banknote",
    "💸": "money with wings", "💳": "credit card", "🧾": "receipt", "💹": "chart increasing with yen", "✉": "envelope",
    "📧": "e-mail", "📨": "incoming envelope", "📩": "envelope with arrow", "📤": "outbox tray", "📥": "inbox tray",
    "📦": "package", "📫": "closed mailbox with raised flag", "📪": "closed mailbox with lowered flag",
    "📬": "open mailbox with raised flag", "📭": "open mailbox with lowered flag", "📮": "postbox", "🗳": "ballot box with ballot",
    "✏": "pencil", "✒": "black nib", "🖋": "fountain pen", "🖊": "pen", "🖌": "paintbrush", "🖍": "crayon", "📝": "memo",
    "💼": "briefcase", "📁": "file folder", "📂": "open file folder", "🗂": "card index dividers", "📅": "calendar",
    "📆": "tear-off calendar", "🗒": "spiral notepad", "🗓": "spiral calendar", "📇": "card index", "📈": "chart increasing",
    "📉": "chart decreasing", "📊": "bar chart", "📋": "clipboard", "📌": "pushpin", "📍": "round pushpin", "📎": "paperclip",
    "🖇": "linked paperclips", "📏": "straight ruler", "📐": "triangular ruler", "✂": "scissors", "🗃": "card file box",
    "🗄": "file cabinet", "🗑": "wastebasket", "🔒": "locked", "🔓": "unlocked", "🔏": "locked with pen", "🔐": "locked with key",
    "🔑": "key", "🗝": "old key", "🔨": "hammer", "⛏": "pick", "⚒": "hammer and pick", "🛠": "hammer and wrench", "🗡": "dagger",
    "⚔": "crossed swords", "💣": "bomb", "🏹": "bow and arrow", "🛡": "shield", "🔧": "wrench", "🔩": "nut and bolt", "⚙": "gear",
    "🗜": "clamp", "⚖": "balance scale", "🦯": "white cane", "🔗": "link", "⛓": "chains", "🧰": "toolbox", "🧲": "magnet",
    "⚗": "alembic", "🧪": "test tube", "🧫": "petri dish", "🧬": "dna", "🔬": "microscope", "🔭": "telescope",
    "📡": "satellite antenna", "💉": "syringe", "🩸": "drop of blood", "💊": "pill", "🩹": "adhesive bandage",
    "🩺": "stethoscope", "🚪": "door", "🛗": "elevator", "🛏": "bed", "🛋": "couch and lamp", "🚽": "toilet", "🚿": "shower",
    "🛁": "bathtub", "🧴": "lotion bottle", "🧷": "safety pin", "🧹": "broom", "🧺": "basket", "🧻": "roll of paper", "🧼": "soap",
    "🧽": "sponge", "🧯": "fire extinguisher", "🛒": "shopping cart", "🚬": "cigarette", "⚰": "coffin", "🪦": "headstone",
    "⚱": "funeral urn", "🧿": "nazar amulet", "🪬": "hamsa", "🗿": "moai", "🪧": "placard", "🪪": "identification card",
    "🏧": "ATM sign", "🚮": "litter in bin sign", "🚰": "potable water", "♿": "wheelchair symbol", "🚹": "men’s room",
    "🚺": "women’s room", "🚻": "restroom", "🚼": "baby symbol", "🚾": "water closet", "🛂": "passport control", "🛃": "customs",
    "🛄": "baggage claim", "🛅": "left luggage", "⚠": "warning", "🚸": "children crossing", "⛔": "no entry", "🚫": "prohibited",
    "🚳": "no bicycles", "🚭": "no smoking", "🚯": "no littering", "🚱": "non-potable water", "🚷": "no pedestrians",
    "📵": "no mobile phones", "🔞": "no one under eighteen", "☢": "radioactive", "☣": "biohazard", "⬆": "up arrow",
    "↗": "up-right arrow", "➡": "right arrow", "↘": "down-right arrow", "⬇": "down arrow", "↙": "down-left arrow",
    "⬅": "left arrow", "↖": "up-left arrow", "↕": "up-down arrow", "↔": "left-right arrow", "↩": "right arrow curving left",
    "↪": "left arrow curving right", "⤴": "right arrow curving up", "⤵": "right arrow curving down", "🔃": "clockwise vertical arrows",
    "🔄": "counterclockwise arrows button", "🔙": "BACK arrow", "🔚": "END arrow", "🔛": "ON! arrow", "🔜": "SOON arrow",
    "🔝": "TOP arrow", "🛐": "place of worship", "⚛": "atom symbol", "🕉": "om", "✡": "star of David", "☸": "wheel of dharma",
    "☯": "yin yang", "✝": "latin cross", "☦": "orthodox cross", "☪": "star and crescent", "☮": "peace symbol", "🕎": "menorah",
    "🔯": "dotted six-pointed star", "♈": "Aries", "♉": "Taurus", "♊": "Gemini", "♋": "Cancer", "♌": "Leo", "♍": "Virgo",
    "♎": "Libra", "♏": "Scorpio", "♐": "Sagittarius", "♑": "Capricorn", "♒": "Aquarius", "♓": "Pisces", "⛎": "Ophiuchus",
    "🔀": "shuffle tracks button", "🔁": "repeat button", "🔂": "repeat single button", "▶": "play button",
    "⏩": "fast-forward button", "⏭": "next track button", "⏯": "play or pause button", "◀": "reverse button",
    "⏪": "fast reverse button", "⏮": "last track button", "🔼": "upwards button", "⏫": "fast up button",
    "🔽": "downwards button", "⏬": "fast down button", "⏸": "pause button", "⏹": "stop button", "⏺": "record button",
    "⏏": "eject button", "🎦": "cinema", "🔅": "dim button", "🔆": "bright button", "📶": "antenna bars", "🛜": "wireless",
    "📳": "vibration mode", "📴": "mobile phone off", "♀": "female sign", "♂": "male sign", "⚧": "transgender symbol",
    "✖": "multiply", "➕": "plus", "➖": "minus", "➗": "divide", "🟰": "heavy equals sign", "♾": "infinity",
    "‼": "double exclamation mark", "⁉": "exclamation question mark", "❓": "red question mark", "❔": "white question mark",
    "❕": "white exclamation mark", "❗": "red exclamation mark", "〰": "wavy dash", "💱": "currency exchange",
    "💲": "heavy dollar sign", "⚕": "medical symbol", "♻": "recycling symbol", "⚜": "fleur-de-lis", "🔱": "trident emblem",
    "📛": "name badge", "🔰": "Japanese symbol for beginner", "⭕": "hollow red circle", "✅": "check mark button",
    "☑": "check box with check", "✔": "check mark", "❌": "cross mark", "❎": "cross mark button", "➰": "curly loop",
    "➿": "double curly loop", "〽": "part alternation mark", "✳": "eight-spoked asterisk", "✴": "eight-pointed star",
    "❇": "sparkle", "©": "copyright", "®": "registered", "™": "trade mark", "🔠": "input latin uppercase",
    "🔡": "input latin lowercase", "🔢": "input numbers", "🔣": "input symbols", "🔤": "input latin letters",
    "🅰": "A button (blood type)", "🆎": "AB button (blood type)", "🅱": "B button (blood type)", "🆑": "CL button",
    "🆒": "COOL button", "🆓": "FREE button", "ℹ": "information", "🆔": "ID button", "Ⓜ": "circled M", "🆕": "NEW button",
    "🆖": "NG button", "🅾": "O button (blood type)", "🆗": "OK button", "🅿": "P button", "🆘": "SOS button", "🆙": "UP! button",
    "🆚": "VS button", "🈁": "Japanese “here” button", "🈂": "Japanese “service charge” button", "🈷": "Japanese “monthly amount” button",
    "🈶": "Japanese “not free of charge” button", "🈯": "Japanese “reserved” button", "🉐": "Japanese “bargain” button",
    "🈹": "Japanese “discount” button", "🈚": "Japanese “free of charge” button", "🈲": "Japanese “prohibited” button",
    "🉑": "Japanese “acceptable” button", "🈸": "Japanese “application” button", "🈴": "Japanese “passing grade” button",
    "🈳": "Japanese “vacancy” button", "㊗": "Japanese “congratulations” button", "㊙": "Japanese “secret” button",
    "🈺": "Japanese “open for business” button", "🈵": "Japanese “no vacancy” button", "🔴": "red circle", "🟠": "orange circle",
    "🟡": "yellow circle", "🟢": "green circle", "🔵": "blue circle", "🟣": "purple circle", "🟤": "brown circle",
    "⚫": "black circle", "⚪": "white circle", "🟥": "red square", "🟧": "orange square", "🟨": "yellow square",
    "🟩": "green square", "🟦": "blue square", "🟪": "purple square", "🟫": "brown square", "⬛": "black large square",
    "⬜": "white large square", "◼": "black medium square", "◻": "white medium square", "◾": "black medium-small square",
    "◽": "white medium-small square", "▪": "black small square", "▫": "white small square", "🔶": "large orange diamond",
    "🔷": "large blue diamond", "🔸": "small orange diamond", "🔹": "small blue diamond", "🔺": "red triangle pointed up",
    "🔻": "red triangle pointed down", "💠": "diamond with a dot", "🔘": "radio button", "🔳": "white square button",
    "🔲": "black square button", "🏁": "chequered flag", "🚩": "triangular flag", "🎌": "crossed flags", "🏴": "black flag",
}

# Search words beyond the name: chat shortcodes and everyday words.
KEYWORDS = {
    "😀": "grinning happy smile", "😃": "smiley happy", "😄": "smile happy", "😁": "grin", "😆": "laughing satisfied haha",
    "😅": "sweat_smile phew", "🤣": "rofl lol laughing", "😂": "joy lol laughing haha tears", "🙂": "slight_smile smile",
    "🙃": "upside_down sarcasm", "🫠": "melting", "😉": "wink", "😊": "blush happy", "😇": "innocent halo angel",
    "🥰": "love hearts adore", "😍": "heart_eyes love crush", "🤩": "star_struck wow amazing", "😘": "kissing_heart kiss",
    "😗": "kissing kiss", "☺": "relaxed smile", "😚": "kiss", "😙": "kiss", "🥲": "smiling_tear grateful", "😋": "yum tasty delicious",
    "😛": "tongue", "😜": "wink tongue crazy", "🤪": "crazy goofy wild", "😝": "tongue", "🤑": "money rich", "🤗": "hug hugging",
    "🤭": "oops giggle", "🫢": "gasp surprised", "🫣": "peeking shy", "🤫": "shush quiet secret", "🤔": "thinking hmm think",
    "🫡": "salute respect", "🤐": "zipper secret quiet", "🤨": "raised_eyebrow skeptical sus", "😐": "neutral meh", "😑": "expressionless meh",
    "😶": "no_mouth speechless", "🫥": "dotted invisible", "😏": "smirk", "😒": "unamused meh", "🙄": "eye_roll rolling_eyes whatever",
    "😬": "grimacing awkward yikes", "😮‍💨": "exhale sigh relief", "🤥": "lying liar", "🫨": "shaking", "😌": "relieved calm",
    "😔": "pensive sad", "😪": "sleepy", "🤤": "drool", "😴": "sleeping zzz tired", "😷": "mask sick", "🤒": "sick fever ill",
    "🤕": "hurt injured", "🤢": "nauseated sick gross", "🤮": "vomit puke gross", "🤧": "sneeze sick", "🥵": "hot sweating",
    "🥶": "cold freezing", "🥴": "woozy drunk tipsy", "😵": "dizzy dead", "😵‍💫": "dizzy spiral", "🤯": "mind_blown exploding shocked",
    "🤠": "cowboy yeehaw", "🥳": "party partying celebrate birthday", "🥸": "disguise", "😎": "sunglasses cool", "🤓": "nerd geek",
    "🧐": "monocle curious", "😕": "confused", "🫤": "diagonal meh", "😟": "worried", "🙁": "frown sad", "☹": "frowning sad",
    "😮": "open_mouth wow surprised", "😯": "hushed surprised", "😲": "astonished shocked wow", "😳": "flushed embarrassed",
    "🥺": "pleading puppy please", "🥹": "holding_back_tears touched", "😦": "frowning", "😧": "anguished", "😨": "fearful scared",
    "😰": "anxious nervous", "😥": "disappointed_relieved", "😢": "cry sad tear", "😭": "sob crying sad", "😱": "scream scared horror",
    "😖": "confounded", "😣": "persevere", "😞": "disappointed sad", "😓": "sweat", "😩": "weary tired", "😫": "tired exhausted",
    "🥱": "yawn bored tired", "😤": "triumph huff angry", "😡": "rage angry mad", "😠": "angry mad", "🤬": "cursing swearing angry",
    "😈": "devil smiling_imp evil", "👿": "imp devil angry", "💀": "skull dead dying lmao", "☠": "skull_crossbones danger death",
    "💩": "poop shit", "🤡": "clown", "👹": "ogre monster", "👺": "goblin", "👻": "ghost boo halloween", "👽": "alien ufo",
    "👾": "space_invader game alien", "🤖": "robot bot", "😺": "cat", "😸": "cat", "😹": "cat joy", "😻": "cat love",
    "🙈": "see_no_evil monkey shy", "🙉": "hear_no_evil monkey", "🙊": "speak_no_evil monkey oops", "💔": "broken_heart heartbreak",
    "❤": "heart love red", "🧡": "orange_heart", "💛": "yellow_heart", "💚": "green_heart", "💙": "blue_heart", "💜": "purple_heart",
    "🖤": "black_heart", "🤍": "white_heart", "🤎": "brown_heart", "🩷": "pink_heart", "🩵": "light_blue_heart", "🩶": "grey_heart",
    "💕": "two_hearts love", "💖": "sparkling_heart love", "💗": "heartpulse love", "❤‍🔥": "heart_on_fire passion",
    "💯": "100 hundred perfect", "💥": "boom collision", "💢": "anger", "💤": "zzz sleep", "💬": "speech chat message",
    "💦": "sweat_drops water splash", "💨": "dash wind fast", "👋": "wave hi hello bye", "👌": "ok_hand ok perfect",
    "🤌": "pinched italian", "✌": "v peace victory", "🤞": "fingers_crossed luck hope", "🤟": "love_you", "🤘": "metal rock horns",
    "🤙": "call_me shaka", "👈": "point_left", "👉": "point_right", "👆": "point_up", "👇": "point_down", "☝": "point_up",
    "🖕": "middle_finger", "🫵": "you point", "👍": "thumbsup +1 like yes ok good", "👎": "thumbsdown -1 dislike no bad",
    "✊": "fist", "👊": "punch fist bump", "👏": "clap applause bravo", "🙌": "raised_hands praise hooray", "🫶": "heart_hands love",
    "🙏": "pray please thanks thank_you", "🤝": "handshake deal", "💪": "muscle strong flex", "👀": "eyes look watching",
    "🧠": "brain smart", "🤷": "shrug idk", "🤦": "facepalm", "🙋": "raising_hand hi", "🙇": "bow sorry", "🫂": "hug people_hugging",
    "👑": "crown king queen", "💎": "gem diamond", "💍": "ring", "💋": "kiss lips", "🎉": "tada party congratulations celebrate",
    "🎊": "confetti party", "🎂": "cake birthday", "🎁": "gift present", "🏆": "trophy win winner", "🥇": "first gold medal",
    "🔥": "fire lit hot flame", "⭐": "star", "🌟": "star glowing", "✨": "sparkles shiny", "🌈": "rainbow", "☀": "sun sunny",
    "⚡": "zap lightning", "❄": "snow cold", "🌊": "ocean wave water", "💧": "droplet water", "🍕": "pizza", "🍔": "burger hamburger",
    "🍟": "fries", "🍺": "beer", "🍻": "beers cheers", "🥂": "champagne toast cheers", "🍷": "wine", "☕": "coffee tea",
    "🍿": "popcorn", "🍩": "donut doughnut", "🍪": "cookie", "🍰": "cake", "🍣": "sushi", "🌮": "taco", "🍆": "eggplant",
    "🍑": "peach", "🍉": "watermelon", "🍓": "strawberry", "🍎": "apple", "🥑": "avocado", "🎮": "video_game gaming game controller",
    "🕹": "joystick gaming", "🎧": "headphones music", "🎤": "mic microphone sing", "🎵": "music note", "🎶": "music notes",
    "🎸": "guitar music", "💻": "computer laptop pc", "🖥": "desktop computer pc", "⌨": "keyboard", "🖱": "mouse", "📱": "phone mobile",
    "📞": "phone call", "🔊": "speaker loud volume", "🔇": "mute silent", "🔔": "bell notification", "💡": "bulb idea",
    "💰": "money_bag money", "💸": "money_wings money spend", "💵": "dollar money", "📌": "pin pushpin", "📎": "paperclip",
    "🔒": "lock locked", "🔑": "key", "🔨": "hammer", "🛠": "tools", "⚙": "gear settings", "💣": "bomb", "🚀": "rocket launch",
    "✈": "airplane plane flight", "🚗": "car", "🏠": "house home", "🌍": "earth world globe", "🌙": "moon night",
    "✅": "white_check_mark check done yes ok", "☑": "check", "✔": "check tick", "❌": "x cross no wrong", "❎": "x cross",
    "⚠": "warning", "❓": "question", "❗": "exclamation important", "‼": "bangbang", "🆗": "ok", "🆒": "cool", "🆕": "new",
    "🆘": "sos help", "🔴": "red_circle", "🟢": "green_circle", "🔵": "blue_circle", "⚫": "black_circle", "⚪": "white_circle",
    "🐶": "dog puppy", "🐱": "cat kitten", "🐭": "mouse", "🐹": "hamster", "🐰": "bunny rabbit", "🦊": "fox", "🐻": "bear",
    "🐼": "panda", "🐨": "koala", "🐯": "tiger", "🦁": "lion", "🐮": "cow", "🐷": "pig", "🐸": "frog", "🐵": "monkey",
    "🐔": "chicken", "🐧": "penguin", "🐦": "bird", "🦆": "duck", "🦅": "eagle", "🦉": "owl", "🦄": "unicorn", "🐝": "bee",
    "🐛": "bug", "🦋": "butterfly", "🐌": "snail slow", "🐍": "snake", "🐢": "turtle slow", "🐙": "octopus", "🦀": "crab",
    "🐟": "fish", "🐬": "dolphin", "🐳": "whale", "🦈": "shark", "🐊": "crocodile", "🦖": "dinosaur t-rex", "🐉": "dragon",
    "🌹": "rose flower", "🌸": "cherry_blossom flower", "🌻": "sunflower", "🍀": "four_leaf_clover luck lucky", "🌲": "tree",
    "🌵": "cactus", "🍁": "maple_leaf autumn", "🎃": "jack_o_lantern halloween pumpkin", "🎄": "christmas_tree xmas",
    "🎅": "santa christmas", "⚽": "soccer football ball", "🏀": "basketball", "🎯": "dart bullseye target", "🎲": "dice game_die",
    "♟": "chess", "🃏": "joker card", "🏁": "checkered_flag finish race", "🚩": "red_flag", "🏳‍🌈": "rainbow_flag pride",
    "🏴‍☠": "pirate", "🥷": "ninja", "🧙": "wizard mage", "🧛": "vampire", "🧟": "zombie", "🦸": "superhero",
    "👶": "baby", "👨‍💻": "developer coder programmer", "🧑‍💻": "developer coder programmer", "👩‍💻": "developer coder programmer",
    "🦾": "robot arm", "🗿": "moai stone", "🧊": "ice cube", "🧂": "salt", "🩸": "blood", "💊": "pill medicine",
    "💉": "syringe vaccine", "🚬": "smoking cigarette", "⚰": "coffin dead", "🪦": "rip grave headstone",
}


# ---- building -------------------------------------------------------------------------------------

def is_text_default(cp):
    if cp < 0x1F000:
        return cp not in EP_YES_LOW
    return cp in EP_NO_HIGH


def is_component(cp):
    return cp in (ZWJ, VS16, VS15, KEYCAP) or cp in TONES or 0xE0020 <= cp <= 0xE007F


def qualify(cps):
    """Fully qualified form: U+FE0F after every text-default base that isn't followed by a skin tone."""
    plain = [c for c in cps if c not in (VS16, VS15)]
    out = []
    for i, cp in enumerate(plain):
        out.append(cp)
        if is_component(cp) or 0x1F1E6 <= cp <= 0x1F1FF:
            continue
        nxt = plain[i + 1] if i + 1 < len(plain) else None
        if is_text_default(cp) and nxt not in TONES:
            out.append(VS16)
    return out


def parse_token(token):
    name = None
    if "|" in token:
        token, name = token.split("|", 1)
    if token.startswith("kc:"):
        cps = [ord(token[3]), KEYCAP]
        name = name or "keycap: " + token[3]
    elif token.startswith("ri:"):
        cps = [0x1F1E6 + ord(c) - ord("A") for c in token[3:]]
        name = name or "flag: " + token[3:]
    elif token.startswith("tag:"):
        cps = [0x1F3F4] + [0xE0000 + ord(c) for c in token[4:]] + [0xE007F]
    else:
        cps = []
        for part in token.split("+"):
            if cps:
                cps.append(ZWJ)
            cps += [ord(c) for c in part if ord(c) not in (VS16, VS15)]
    return qualify(cps), name


def formal_name(cps):
    base = [c for c in cps if c not in (VS16, VS15)]
    if len(base) != 1:
        return None
    try:
        return unicodedata.name(chr(base[0])).lower()
    except ValueError:
        return None


def key_of(cps):
    return "".join(chr(c) for c in cps if c not in (VS16, VS15))


def tone_variants(cps):
    """The five skin-tone forms of a base sequence, or [] if it can't have them."""
    plain = [c for c in cps if c not in (VS16, VS15)]
    if not plain or plain[0] not in MODIFIER_BASES:
        return []
    parts = []
    current = []
    for c in plain:
        if c == ZWJ:
            parts.append(current)
            current = []
        else:
            current.append(c)
    parts.append(current)
    persons = [i for i, p in enumerate(parts) if p and p[0] in PERSONS]
    couple = len(parts) > 1 and any(p and p[0] in (0x1F91D, 0x2764) for p in parts)
    family = len(parts) > 1 and all(p and p[0] in PERSONS for p in parts)
    if family:
        return []
    targets = persons if couple else [0]
    out = []
    for tone in TONES:
        seq = []
        for i, p in enumerate(parts):
            if i:
                seq.append(ZWJ)
            if i in targets:
                seq += [p[0], tone] + p[1:]
            else:
                seq += p
        out.append(qualify(seq))
    return out


def tokens(text):
    """Whitespace-separated tokens; the words of a "|name" run on until the next emoji."""
    out = []
    for word in text.split():
        starts_emoji = ord(word[0]) > 0x7F or word.startswith(("kc:", "ri:", "tag:"))
        if out and not starts_emoji and "|" in out[-1]:
            out[-1] += " " + word
        else:
            out.append(word)
    return out


def candidates():
    """[(group, cps, name, keywords, tones)] in order; tones: list of the 5 variants' cps."""
    seen = set()
    out = []
    for list_index, text in enumerate(LISTS):
        group = LIST_GROUP[list_index]
        for token in tokens(text):
            cps, name = parse_token(token)
            key = key_of(cps)
            if key in seen:
                continue
            seen.add(key)
            glyph = "".join(chr(c) for c in cps if c not in (VS16, VS15))
            if not name:
                name = NAMES.get(glyph) or formal_name(cps) or "emoji"
            words = KEYWORDS.get(glyph, "").strip()
            out.append((group, cps, name, words, tone_variants(cps)))
    return out


def hexseq(cps):
    return "-".join("%x" % c for c in cps)


def cmd_candidates(path):
    rows = candidates()
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for _, cps, _, _, tones in rows:
            f.write(hexseq(cps) + "\n")
            for t in tones:
                f.write(hexseq(t) + "\n")
    print("%d candidates (%d with skin tones) -> %s" % (len(rows), sum(1 for r in rows if r[4]), path))


def c_bytes(text):
    return '"' + "".join("\\x%02X" % b for b in text.encode("utf-8")) + '"'


def c_ascii(text):
    out = []
    for ch in text:
        if ch in '"\\':
            out.append("\\" + ch)
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append("".join("\\x%02X" % b for b in ch.encode("utf-8")) + '" "')
    return '"' + "".join(out) + '"'


def cmd_table(probe, path, unfiltered=False):
    ok = {}
    if not unfiltered:
        with open(probe, encoding="ascii") as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2:
                    ok[parts[0]] = parts[1] == "1"
    rows = candidates()
    lines = []
    bases = 0
    dropped = []
    tones_kept = 0
    for group, cps, name, words, tones in rows:
        if not unfiltered and not ok.get(hexseq(cps), False):
            dropped.append(hexseq(cps) + " " + name)
            continue
        good_tones = [t for t in tones if unfiltered or ok.get(hexseq(t), False)]
        has_tones = len(tones) == 5 and len(good_tones) == 5
        flags = 0
        plain = [c for c in cps if c not in (VS16, VS15)]
        if len(plain) == 1 and is_text_default(plain[0]):
            flags |= 1
        if has_tones:
            flags |= 2
        text = "".join(chr(c) for c in cps)
        lines.append("    {%s, %s, %d, %d, %s}," % (c_bytes(text), c_ascii(name), group, flags, c_ascii(words)))
        bases += 1
        if has_tones:
            tones_kept += 1
            for i, t in enumerate(good_tones):
                lines.append("    {%s, \"\", %d, %d, \"\"}," % (c_bytes("".join(chr(c) for c in t)), group, 4 * (i + 1)))
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("// Generated by tools/emoji/gen_emoji.py (Python %s, Unicode %s): do not edit.\n" % (sys.version.split()[0], unicodedata.unidata_version))
        f.write("// %d emoji in the picker, %d of them with skin tones; %d table rows. Filtered by tools/emoji_probe%s.\n"
                % (bases, tones_kept, len(lines), " (unfiltered bootstrap)" if unfiltered else ""))
        f.write("// Row: UTF-8 text (fully qualified), name, group, flags (1 text default, 2 has tones, 4*n tone n), search words.\n")
        f.write("static const Row kRows[] = {\n")
        f.write("\n".join(lines))
        f.write("\n};\n")
    print("%d emoji (%d with tones), %d rows -> %s; %d candidates dropped" % (bases, tones_kept, len(lines), path, len(dropped)))
    if dropped and len(sys.argv) > 4 and sys.argv[4] == "-v":
        print("\n".join(dropped))


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "candidates":
        cmd_candidates(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "table":
        cmd_table(sys.argv[2], sys.argv[3], unfiltered=sys.argv[2] == "--unfiltered")
    else:
        print(__doc__ or "usage: gen_emoji.py candidates <out> | table <probe|--unfiltered> <out.inc>")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
