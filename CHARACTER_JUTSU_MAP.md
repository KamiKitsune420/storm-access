# Character ↔ jutsu-id map (character select naming)

How Storm Access names the hovered fighter. The fighter NAME is a portrait
graphic (never decoded), so we identify fighters by the jutsu ids their info
panel decodes on hover: signature (`c_jyu_`/`c_com_`), **ultimate** (`c_ult_`),
form tag (`c_cha_`/`c_costume_`). The **ultimate is the reliable key** (unique per
fighter-form); signatures are a fallback and are only mapped when unique. Live
table lives in `Accessibility.cpp` → `CHAR_NAMES[]`; keep this doc in sync.

## Rules learned
- **Shared signatures — do NOT key on them** (map the ultimate instead):
  - `c_jyu_000` "Rasengan" → Naruto AND Minato
  - `c_jyu_004` "Fire Style: Fire Ball Jutsu" → Itachi AND young Sasuke
  - `c_jyu_023` "Water Style: Water Dragon Jutsu" → Zabuza AND Suigetsu
- An unmapped fighter falls back to speaking its signature jutsu (never a wrong
  name). Ougi voice-line ids encode the character (e.g. `3mnt`=Minato,
  `5kgy`=Kaguya, `6ssk`=Sasuke, `3nrt`=Naruto) — handy for confirming ids.

## Confirmed id → character (as of this pass)
- **Naruto** — ult 147/178/02/00/28/119/145/179; sig 002/323/515/555
- **Sasuke** — ult 173/153/59/184; sig 557/324/142/007
- **Sakura** — ult 152; sig 549
- **Itachi** — ult 33
- **Kakashi** — ult 180; sig 059
- **Madara** — ult 174/154; sig com_1367, 386
- **Minato** — ult 156 (Flying Raijin)
- **Kushina** — sig 381 (Frying Pan Attack)
- **Hashirama** — ult 157; sig com_1030
- **Tobirama** — ult 71
- **Hiruzen** — ult 106
- **Tsunade** — ult 29; sig 030 (Heaven Kick of Pain)
- **Gaara** — ult 135/99
- **Ino** — ult 12; sig 016
- **Choji** — ult 11; sig 015
- **Sai** — ult 06
- **Might Guy** — ult 169 (Night Guy)
- **Tenten** — ult 18/09; sig 013
- **Zabuza** — ult 68
- **Suigetsu** — ult 131/40
- **Jugo** — ult 41 (Living Wall Fist); sig 046
- **Orochimaru** — ult 30
- **Kabuto** — ult 139 (Sage Art: Inorganic Reanimation)
- **Sasori** — ult 75
- **Chiyo** — ult 26
- **Mei Terumi** — ult 116 (Lava Style)
- **Ohnoki** — ult 66 (Particle Style)
- **Fourth Raikage (Ay)** — ult 58 (Liger Bomb)
- **Third Raikage** — ult 143 (Piercing Thrust of Hell); sig 316
- **Third Kazekage** — ult 36 (Iron Sand)
- **Fourth Kazekage** — ult 144 (Gold Dust); sig 318
- **Kakuzu** — ult 38
- **Kisame** — ult 118 (Shark Bomb); sig 034
- **Obito** — ult 168 (Sword of Nunoboko); sig 519
- **Kaguya** — ult 172 (Final Truthseeker Orbs); sig 529 (Eighty Gods Vacuum Fists)
- **Nagato** — ult 121 (Summoning: Gedo Statue)
- **Fuu** — ult 138 (Scale Powder Blast); sig 260 (Fan Slash)
- **Yugito** — ult 125 (Cat Fire Bowl); sig 257 (Mouse Hairball)
- **Shikamaru** — ult 10 (Shadow Pull); sig 014 (Shadow Stitching)
- **Yagura** — ult 129 (Rough Sea Spume); sig 312 (Rising Coral Ripples)
- **Shisui** — ult 150 (Kotoamatsukami); sig 376 (Uchiha Style: Halo Dance)
- **Obito** (war/masked) — ult 148 (Summoning: Ten Tails); sig com_1118 (Bomb Blast Dance)

## 4th pass added (full sweep): Neji, Hinata, Kiba, Shino, Asuma, Yamato, Danzo,
Darui, Utakata, Kabuto (Snake Cloak, ult 76/31), base Kakashi (ult 72), Taka &
child Sasuke (ult 03 Kirin / 73 Phoenix Flower), Yagura (ult 182), Kushina (ult
149 Red-Hot Habanero). Also removed shared sig `c_jyu_007` "Chidori"
(Sasuke+Kakashi) — key by ultimate.

## 5th pass (user-confirmed): Roshi (ult 126 Great Blazing Eruption / sig 258 Lava
Style: Scorching Rocks); Han (ult 127 Five-Mountain Jump — the non-lava jinchuriki);
Rin Nohara (ult 182 Three Tails Rampage / sig 526 Rolling Logs — was mislabeled
Yagura); Chakra-Mode Naruto (sig 268 Tailed Beast Bomb — decodes NO ultimate, so
code now treats a form tag as a fighter signal + merges multi-signature hovers).

## 6th pass (user-confirmed by ear): Iruka Umino (ult 146 Love Roar / sig 374
Perimeter Barrier), Killer Bee (sig 082 Rising Bomber + Shark Skin), Rock Lee
(com 048 Leaf Hurricane / sig 061 Hidden Lotus), child Hinata (sig 125 Dashing
Double Palm / 019 Protective 64 Palms), Tenten (ult 91 Million Blade Chaos).

## Still unidentified (need user confirm)
- `c_jyu_047`/`c_ult_54` — "Perfume Spray" / "Severe Stinging Slap" (a kunoichi?)
- child Obito: user says a fire-style child Obito is unnamed. `c_ult_73` "Phoenix
  Flower" (+ Fire Ball) is currently labeled child Sasuke — confirm which is which.

## UI layout (from online): character select is 6 pages — 5 of fighters + 1
presets — with empty slots at the end of the roster. A full sweep = all 5 pages.

## Name style (user pref): FULL / CLAN names — "Naruto Uzumaki", "Kaguya
Otsutsuki", "Shisui Uchiha", "Shikamaru Nara". Title-only characters with no clan
stay as titles: "Third Raikage", "Fourth Kazekage", "Third Kazekage". Form tag
(Sage Mode, Reanimation, ...) is still appended from c_cha/c_costume.

## Full roster (108, from PowerPyx — target names + forms)
Naruto (Sage of the Six Paths / Kurama Link / Sage Mode / base); Sakura; Sai;
Kakashi; Might Guy; Yamato; Shikamaru; Choji; Ino; Neji; Rock Lee; Tenten; Kiba;
Shino; Hinata; Asuma; Konohamaru; Shisui; Iruka; Kushina; Jiraiya; Kakashi;
Obito; Rin; Sasuke (Rinne Sharingan / Eternal Mangekyo / Five Kage Summit / Taka /
base); Orochimaru; Kabuto (Sage Mode / Snake Cloak / base); Suigetsu; Jugo; Karin;
Hashirama; Tobirama; Hiruzen; Minato; Tsunade; Danzo; Gaara; Kankuro; Temari;
Ay (Raikage); Killer Bee; Darui; Ohnoki; Mei Terumi; Mifune; Madara (Six Paths /
base); Kaguya; Obito (Ten Tails Jinchuriki / base); Tobi (Great Ninja War / base);
Masked Man; Pain; Nagato; Konan; Itachi (base / Reanimation); Kisame; Kakuzu;
Hidan; Deidara; Sasori; Fourth Kazekage; Third Raikage; Second Tsuchikage; Second
Mizukage; Hanzo; Chiyo; Zabuza; Haku; Yugito; Yagura; Roshi; Han; Utakata; Fuu;
child versions of the Konoha 11 + Gaara/Kankuro/Temari/Kimimaro; Naruto (The Last);
Sasuke (The Last); Hanabi; Boruto; Sarada.

Sources: PowerPyx (roster/forms), Naruto Storm wiki & Narutopedia (jutsu → char).
