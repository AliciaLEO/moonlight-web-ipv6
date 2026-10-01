# Banc d'encodeur du host natif — campagne du 04/09/2026

> Phase E du plan v2. Instrument : `MoonlightWeb.exe --native-bench` (design
> §14), qui capture, convertit et encode un écran vers un puits — pas de réseau,
> pas de navigateur — et donne par frame l'heure de chaque étape, la taille et
> le QP moyen. Ce document est le **livrable avant décision** : les chiffres,
> ce qu'ils disent, une recommandation. Rien n'est appliqué au produit avant
> confirmation (règle 0.2.2 du plan).

## 1. Banc

| | |
|---|---|
| Machine | bench-desk : 2× RTX 5060 Ti (pilote 32.0.15.9636), iGPU AMD Radeon (Ryzen, AMF, pilote 32.0.21045), Windows 11 |
| Écran capturé | Display 3 = écran virtuel VDD 2560×1440 @ 165 Hz sur la RTX 5060 Ti n° 1 |
| Contenus | **défilement** : page de texte serif qui défile à 600 px/s, pilotée par `requestAnimationFrame` (une image par rafraîchissement, ~160 présents/s) · **jeu** : clip Super Mario Galaxy 1080p 60 fps étiré plein écran, relancé de zéro avant chaque passe (démarrage 3 s après chargement, banc lancé 3,5 s après) · **fixe** : la même page de texte sans mouvement |
| Réglage de référence | 2560×1440, fps = celui de l'écran (165), CBR 40 Mbit/s, HEVC 4:2:0, keyframes à la demande, 10 s par passe |
| Reproductibilité | réglage courant répété en tête et en queue de chaque matrice : encode 4,67 / 4,65 / 4,67 / 4,69 ms (défilement), 6,55 / 6,52 ms (jeu) |
| Colonnes | `encode` = t₂ converti → t₃ bitstream lisible, moyenne / p95 / p99 ms · `total` = t₀ présent → t₃ · `Ko` = octets par frame delta, moyenne · `QP` = quantificateur moyen rapporté par l'encodeur (plus bas = plus net ; q-index 0–255 pour l'AV1, pas comparable) |

Ce que le réglage courant vaut, tel que le pilote le livre — lu dans le log, pas
supposé : **NVENC P4 / ultra-low-latency active le multipass quart de résolution,
AQ spatial et temporel éteints, pas de lookahead** ; **AMF ultra-low-latency =
qualité « speed », pré-analyse éteinte, VBAQ éteint**.

## 2. NVENC (RTX 5060 Ti) — défilement de texte, 1440p à 165 présents/s

| Réglage | encode ms (moy / p95 / p99) | total moy | Ko/frame | QP |
|---|---|---|---|---|
| **P4/ULL (courant)** | **4,67 / 5,63 / 6,00** | 5,32 | 28,1 | **26** |
| P1 | 2,71 / 4,10 / 4,61 | 3,14 | 29,4 | 31 |
| P2 | 4,36 / 5,63 / 5,63 | 4,88 | 29,4 | 31 |
| P3 | 4,65 / 5,63 / 6,14 | 5,29 | 28,5 | 27 |
| P5 | 4,65 / 5,63 / 6,14 | 5,28 | 28,3 | 26 |
| P6 | 4,67 / 5,63 / 6,14 | 5,31 | 28,2 | 26 |
| P7 | 4,89 / 5,63 / 6,14 | 5,60 | 28,2 | 26 |
| P4, tuning LL (multipass éteint par le preset) | 4,11 / 5,12 / 5,63 | 4,58 | 27,8 | 26 |
| P4, multipass off | 4,28 / 5,12 / 5,63 | 4,75 | 28,0 | 26 |
| P4, multipass full | 5,08 / 6,14 / 6,14 | 5,94 | 28,6 | 26 |
| P4, AQ spatial | 5,00 / 6,14 / 6,66 | 5,76 | 25,6 | 21 |
| P4, AQ temporel | 4,86 / 6,14 / 6,66 | 5,53 | 28,1 | 26 |
| P4, VBV = 1 frame (29 Ko, sans plancher) | 4,62 / 5,63 / 6,14 | 5,11 | 24,2 | 27 |
| P4, VBV = 2 frames (59 Ko) | 4,67 / 5,63 / 6,14 | 5,26 | 23,7 | 27 |
| P4, H.264 | 4,38 / 5,63 / 5,63 | 4,92 | 28,4 | 27 |
| P4, AV1 | 4,26 / 5,12 / 6,14 | 4,81 | 29,6 | q 90 |
| P4, HEVC 4:4:4 | 4,81 / 5,63 / 6,14 | 5,56 | 28,1 | 26 |
| P4, intra-refresh | 4,74 / 5,63 / 6,14 | 5,44 | 28,2 | 26 |
| P4, fps réglé 60 | 5,56 / 6,66 / 7,17 | 6,03 | 68,2 | 20 |
| **P1, multipass off** | **2,21 / 3,58 / 3,84** | 2,78 | 28,4 | 31 |
| P1, AQ spatial | 3,07 / 4,61 / 5,12 | 3,53 | 29,2 | 25 |
| P1, AV1 | 2,88 / 4,10 / 4,61 | 3,38 | 29,6 | q 138 |
| P1, H.264 | 3,49 / 4,61 / 5,12 | 4,03 | 28,6 | 27 |
| P1, VBV 2 frames | 2,80 / 4,10 / 4,61 | 3,33 | 26,6 | 31 |
| P2, multipass off | 4,03 / 5,12 / 5,63 | 4,67 | 28,2 | 30 |

## 3. NVENC — clip de jeu, 1440p, 60 présents/s

| Réglage | encode ms (moy / p95 / p99) | total moy | Ko/frame | QP |
|---|---|---|---|---|
| **P4/ULL (courant)** | **6,55 / 9,22 / 10,24** | 7,10 | 26,8 | **18** |
| P1 | 3,11 / 4,61 / 4,61 | 3,71 | 27,1 | 19 |
| P2 | 5,30 / 7,17 / 7,17 | 5,86 | 27,4 | 19 |
| P3 | 6,19 / 8,19 / 9,06 | 6,79 | 26,9 | 18 |
| P5 | 6,54 / 9,22 / 10,24 | 7,16 | 27,2 | 18 |
| P6 | 6,64 / 9,22 / 10,24 | 7,22 | 27,1 | 18 |
| P7 | 7,25 / 9,22 / 11,26 | 7,84 | 27,1 | 18 |
| P4, tuning LL | 6,12 / 9,22 / 11,25 | 6,75 | 26,8 | 19 |
| P4, multipass off | 6,12 / 9,22 / 11,26 | 6,69 | 26,7 | 19 |
| P4, multipass full | 8,10 / 12,29 / 13,31 | 8,69 | 27,5 | 19 |
| P4, AQ spatial | 7,17 / 10,24 / 11,26 | 7,74 | 27,4 | 17 |
| P4, AQ temporel | 6,74 / 10,24 / 11,26 | 7,25 | 27,5 | 18 |
| P4, VBV 1 frame | 6,52 / 9,22 / 10,24 | 7,12 | 22,4 | 19 |
| P4, VBV 2 frames | 6,62 / 9,22 / 10,24 | 7,17 | 24,4 | 19 |
| P4, H.264 | 5,58 / 7,68 / 7,68 | 6,11 | 27,1 | 20 |
| P4, AV1 | 5,19 / 7,17 / 7,17 | 5,79 | 25,6 | q 45 |
| P4, HEVC 4:4:4 | 6,90 / 9,22 / 10,24 | 7,49 | 27,4 | 19 |
| P4, intra-refresh | 6,66 / 9,22 / 10,24 | 7,27 | 27,5 | 19 |
| P4, fps réglé 60 | 6,95 / 10,24 / 11,26 | 7,59 | 65,5 | 12 |
| **P1, multipass off** | **2,52 / 3,84 / 4,10** | 3,19 | 26,6 | 19 |
| P1, AQ spatial | 4,14 / 6,14 / 6,66 | 4,79 | 27,2 | 18 |
| P1, AV1 | 3,37 / 4,61 / 5,12 | 4,08 | 25,4 | q 45 |
| P1, H.264 | 3,64 / 5,63 / 5,63 | 4,25 | 27,3 | 20 |
| P1, fps réglé 60 | 3,78 / 5,12 / 5,63 | 4,47 | 64,8 | 13 |
| P1, VBV 2 frames | 3,12 / 4,61 / 4,61 | 3,73 | 24,6 | 19 |
| P2, multipass off | 4,71 / 6,66 / 7,17 | 5,27 | 26,7 | 19 |

## 4. NVENC — écran fixe : la première keyframe et la rafale de raffinement

Ce que l'on regarde ici n'est pas l'encode moyen (rien ne bouge) mais la
**trajectoire des passes de raffinement** (§9.1 du design) : la keyframe, puis
les passes au budget ×3 jusqu'à convergence. Octets et QP des dix premières
frames, 40 Mbit/s, 1440p, VBV 81 Ko (plancher 1/60 s).

| Réglage | keyframe | passes (Ko → QP) | verdict |
|---|---|---|---|
| P4 (courant) | 64 Ko, QP 45 | 163 → 32 · 91 → 25 · 88 → 19 · 77 → 14 · 33 → 12 · 35 → 10 · 33 → **8** | converge en 8 passes |
| P1 | 64 Ko, QP 45 | 149 → 33 · 34 → 31 · 96 → 25 · 63 → 20 · 86 → 15 · 79 → 10 · 19 → **8** | idem, encode 2,2–2,7 ms par passe |
| P1, AQ spatial | 59 Ko, QP 40 | 164 → 26 · 94 → 19 · 89 → 13 · 72 → 9 · 21 → 7 · 33 → **4** | converge, plus bas encore |
| **P1, multipass off** | 89 Ko, QP 45 | 53 → 39 · 9 → 38 · 19 → 36 · 13 → 34 · 25 → 33 · 12 → 31 · 27 → 30 · 12 → **29** | **ne converge pas** : les passes n'utilisent pas le budget, l'image reste à QP 29 |
| P4, VBV 1 frame (29 Ko) | 32 Ko, **QP 50** | 84 → 38 · 88 → 32 · … · 38 → 9 | la première image molle que le plancher de RateControl.h corrige |
| P4, fps réglé 60 | 76 Ko, QP 44 | 184 → 25 · 170 → 15 · 114 → 8 · 63 → 5 · 26 → 4 | converge en 6 passes |

## 5. AMF (iGPU AMD Radeon, par copie inter-GPU)

L'iGPU ne pilote aucun écran : l'encodeur y est atteint par le pont inter-GPU
livré avec ce banc (`gpu=2`, `CrossGpuBridge`), soit une trame de 14 Mo qui
traverse la mémoire système avant la conversion. Le coût du pont est **dans
l'étape convert** (≈ 2 à 5 ms ici) ; la colonne `encode` mesure l'encodeur seul.
**Ce pilote ne rapporte aucun QP** (`GetProperty` échoue sur le buffer de sortie,
comme sur la RX 7600 le 02/09) : la qualité n'a pas de mesure objective côté AMD.

| Contenu · réglage | encode ms (moy / p95 / p99) | total moy | Ko/frame | cadence capturée |
|---|---|---|---|---|
| jeu · speed (courant) | 7,92 / 10,24 / 10,24 | 9,98 | 27,0 | 59,9 fps |
| jeu · balanced | 8,01 / 10,24 / 10,24 | 10,11 | 27,2 | 59,9 |
| jeu · quality | 9,24 / 11,26 / 11,26 | 11,68 | 27,0 | 59,8 |
| jeu · pré-analyse | — | — | — | **la session meurt** (« the AMD encoder stopped producing frames ») |
| jeu · VBAQ | 8,08 / 10,24 / 10,24 | 10,31 | 26,9 | 59,8 |
| jeu · VBV 1 frame | 8,30 / 10,24 / 10,24 | 10,82 | 27,3 | 59,9 |
| jeu · VBV 2 frames | 8,14 / 10,24 / 10,24 | 10,43 | 27,2 | 59,9 |
| jeu · H.264 | 7,92 / 10,24 / 10,24 | 10,18 | 24,3 | 59,9 |
| jeu · intra-refresh | 7,98 / 10,24 / 10,24 | 10,11 | 27,0 | 59,9 |
| jeu · fps réglé 60 | 8,21 / 10,24 / 10,24 | 10,40 | 73,0 | 55,5 |
| jeu · **1920×1080** | 5,94 / 7,68 / 8,19 | 8,05 | 28,0 | 59,9 |
| défilement · speed (courant) | 7,84 / 10,24 / 10,24 | 12,83 | 29,6 | **102 fps** (sur 160 présentés) |
| défilement · balanced | 7,84 / 10,24 / 10,24 | 12,65 | 29,6 | 102 |
| défilement · quality | 9,12 / 11,26 / 11,26 | 14,14 | 29,6 | 89,5 |
| défilement · VBAQ | 7,75 / 10,24 / 10,24 | 12,56 | 29,6 | 104 |
| défilement · H.264 | 7,68 / 9,22 / 10,24 | 12,61 | 29,6 | 104 |
| défilement · 1920×1080 | 5,50 / 7,17 / 7,68 | 9,67 | 29,6 | 136 |
| fixe · speed | keyframe 132 Ko, passes 87 · 77 · 65 · 42 · 80 · 24 Ko puis 0 | | | |

Lecture : sur cet iGPU, les presets AMF ne bougent presque rien (« speed » et
« balanced » sont le même chiffre, « quality » coûte 1,3 ms) ; le VBAQ et le VBV
sont neutres en temps ; la **pré-analyse tue l'encodeur** en ULL/CBR (l'en-tête
AMF la documente pour le VBR à pic contraint seulement). À 1440p l'iGPU **ne
tient pas 165 fps** (7,8 ms d'encode + le pont > 6 ms de période : 102 images/s
capturées sur 160) ; à 1080p il en tient 136. C'est un iGPU : le résultat vaut
pour la classe « portable AMD sans carte », pas pour une RX 7600, qui reste à
mesurer quand elle sera rebranchée.

## 6. Côté client — latence de décodage par codec (mesure A3)

Flux réels depuis l'instance dev (`--dev`), host natif sur Display 3, clip de
jeu, 1440p60 40 Mbit/s, client Chrome sur la même machine, transport
`webrtc-dc-udp`, overlay de stats après 30 s. Moyenne / p99 ms.

| Codec | décodage | rendu | host total | latence affichée |
|---|---|---|---|---|
| HEVC (`hvc1.1.144.L150`) | 1,1 / 2,5 | 0,6 / 2,4 | 6,8 | 9,2 ms |
| AV1 | 0,9 / 2,4 | 0,4 / 0,8 | 5,5 | 7,4 ms |
| H.264 (`avc1.640033`) **avant** correctif | **200,8 / 206,3** | 0,4 / 1,4 | 6,3 | **208 ms** |
| H.264 **après** correctif | 0,9 / 2,7 | 0,4 / 0,8 | 6,1 | 8,3 ms |

Deux bugs trouvés par cette mesure, corrigés dans la foulée (règle 0.2.6 : les
bugs vus au passage se corrigent) :

- **H.264 : 200 ms de décodage.** Le SPS NVENC ne portait pas de
  `bitstream_restriction` ; sans `max_num_reorder_frames`, le décodeur D3D11 de
  Chrome retient un DPB entier avant d'afficher — une douzaine d'images à
  1440p60, sur un flux sans aucune B-frame. `bitstreamRestrictionFlag = 1` dans
  les paramètres VUI : 208 → 8 ms. Le HEVC le portait déjà par défaut.
- **AV1 jamais sélectionnable en natif.** `NativeMediaEngine` testait le masque
  client avec `0x0200`, qui est **HEVC Main10**, pas AV1 (`0x1000`). Un client
  demandant AV1 obtenait du HEVC ; un client demandant du HEVC HDR aurait été lu
  comme demandant de l'AV1. Corrigé par masques par codec en entrée
  (`0xF000` / `0x0F00` / `0x000F`) et format négocié fidèle au profil en sortie.

Aucun codec ne coûte plus de 1 ms de décodage de plus qu'un autre sur ce
Chrome/RTX : le choix de codec peut se faire sur le coût hôte et la licence.

## 6b. Le contenu cible — un FPS en plein écran (Call of Duty, 1440p60)

Ajouté à la demande de Bruno après le premier rapport : le texte qui défile
coûtait +5 de QP à P1, que vaut une scène d'action ? Séquence de gameplay
Call of Duty (vidéo YouTube `yaNr1hHAg2M`, flux 1440p60 VP9 récupéré en local,
lue de 37 s à 52 s, plein écran sur l'écran capturé, relancée de zéro avant
chaque passe — répétabilité du réglage courant 7,67 / 7,69 ms). C'est le cas
le plus dur de la campagne : QP 25 à 40 Mbit/s là où le clip de plateforme
tenait 18.

| Réglage | encode ms (moy / p95 / p99) | total moy | Ko/frame | QP |
|---|---|---|---|---|
| **P4/ULL (courant), 40 Mbit/s** | **7,67 / 10,24 / 11,26** | 8,32 | 29,5 | **25** |
| P1 | 3,40 / 4,61 / 5,12 | 4,02 | 29,5 | **25** |
| P1, AQ spatial | 4,33 / 6,14 / 6,62 | 4,95 | 29,5 | 23 |
| P1, multipass off | 2,62 / 4,10 / 4,61 | 3,21 | 29,0 | 25 |
| P2 | 5,73 / 7,68 / 7,68 | 6,30 | 29,5 | 25 |
| P3 | 6,61 / 8,19 / 8,86 | 7,05 | 29,5 | 25 |
| P4, AQ spatial | 8,32 / 11,26 / 12,29 | 8,87 | 29,5 | 22 |
| P4, AV1 | 5,39 / 7,17 / 7,68 | 5,96 | 28,2 | q 91 |
| P1, AV1 | 3,28 / 4,61 / 5,12 | 3,83 | 28,2 | q 92 |
| P4, H.264 | 5,79 / 7,68 / 7,68 | 6,32 | 29,5 | 27 |
| P1, H.264 | 4,07 / 5,63 / 6,14 | 4,58 | 29,5 | 27 |
| **P4, 20 Mbit/s** | 7,36 / 10,24 / 11,26 | 7,90 | 14,8 | 31 |
| P1, 20 Mbit/s | 3,08 / 4,61 / 4,61 | 3,59 | 14,8 | 32 |
| P1 + AQ, 20 Mbit/s | 4,11 / 5,63 / 6,14 | 4,69 | 14,8 | 29 |

**Sur l'action, P1 ne coûte rien en QP** : 25 contre 25 à 40 Mbit/s, 32 contre
31 à 20 Mbit/s, pour 3,4 ms au lieu de 7,7 (et 3,1 au lieu de 7,4 à 20 Mbit/s).
Le +5 du texte défilant est propre au texte : contours nets à fort contraste,
que P4 sait mieux prédire ; une scène de jeu est faite de textures et de flou de
mouvement, où l'estimation de mouvement plus fine de P4 n'achète rien que le
rate control ne rende sous forme de QP identique.

**Et à l'œil.** Le banc encode vers un puits ; pour juger l'image il faut le
flux décodé. Protocole : instance dev avec `MW_NATIVE_TUNING`, client Chrome
dédié piloté par CDP (clics réels, donc plein écran accordé), flux 1440p60
HEVC 40 Mbit/s du même écran, capture `PrintWindow` de la fenêtre plein écran
en 2560×1440 physiques à 9, 12, 15 et 18 s après le relancement du clip — les
mêmes secondes à ±0,1 s pour P4, P1 et P1 + AQ (`bench/shots/`, montages
`compare-*.png` recadrés au centre et `zoom-*.png` agrandis ×2). Constat : aucun
des trois ne montre de blocs, de fourmillement ni de « bouillie » ; les
contours du décor, les débris, l'arme au premier plan et le HUD sont
également nets ; les différences entre vignettes sont celles du mouvement entre
deux images à un dixième de seconde d'écart, pas du réglage. À égalité de QP,
c'est le résultat attendu. La comparaison à 20 Mbit/s (QP 31–32) reste à faire
à l'œil si Bruno le souhaite ; l'écart d'un point de QP la rend peu probable.

## 6c. Présentateurs client en plein écran 1:1 — clic → drapeau (06/09/2026)

Le 04/09, en fenêtre (1080p → 996×935, donc en réduction), Canvas2D « Off »
mesurait 54 ms de clic → image contre 36 pour FSR1 WebGL2 — un écart assez gros
pour remettre le défaut SDR en question, et assez surprenant pour exiger d'être
recoupé dans le cas qui compte : plein écran, un pixel du flux pour un pixel de
l'écran.

Banc : sonde clic → drapeau (`docs/design/glass-to-glass.md` §5 bis, build
debug), hôte natif **AMF RX 7600, HEVC 2560×1440 @ 60, 20 Mbit/s**, bureau
quasi fixe ; client **Chrome dédié en kiosque** sur l'écran virtuel 1440p de la
même machine (canvas 1707×960 CSS à 1,5 = **2560×1440 physiques**, page visible,
`document.fullscreenElement` vrai), transport `webrtc-dc-udp`. Trois séries de
10 clics par présentateur, **en alternance** (Off, FSR1, Off, FSR1, Off, FSR1),
un clic de chauffe écarté avant chaque série, 60 clics mesurés sur 60.

| Présentateur | séries (médiane ms) | 30 clics : médiane | p90 | min–max |
|---|---|---|---|---|
| Off — Canvas2D `desynchronized` | 27,0 · 36,3 · 31,4 | **34,5** | 62,7 | 20,9–73,5 |
| Auto — FSR1 WebGL2 (EASU + RCAS, ×1) | 32,3 · 35,7 · 29,9 | **34,7** | 67,7 | 20,5–89,2 |

Indiscernables : 0,2 ms de médiane d'écart pour une dispersion de 15 ms entre
séries du même mode. La distribution est bimodale (≈ 25–38 ms ou ≈ 55–70), ce
qui est la quantification de la capture à 60 présents/s sur un bureau immobile
— le drapeau tombe avant ou après l'échéance suivante —, pas le présentateur.
**Verdict : le défaut SDR reste Canvas2D Off.** L'écart vu en fenêtre était
propre à la réduction dans une fenêtre (un `drawImage` qui rééchantillonne vers
le bas contre un passage GL au même coût quelle que soit l'échelle) ; il ne dit
rien du plein écran.

Deux pièges de banc, pour la prochaine fois :

- **L'extension Chrome ne fait pas de plein écran réel.** Elle émule un viewport
  fixe (2048×1017 CSS ici) quel que soit l'écran, `requestFullscreen` répond
  « not granted » à ses clics, et l'onglet reste `hidden`. Toute mesure de
  présentation passe par un Chrome dédié piloté en CDP (`scratchpad/bench/cdp.py`,
  port 9333) où le clic sur le bouton Fullscreen de l'app vaut activation.
- **Un clic injecté qui active une autre fenêtre gèle la sonde.** Le pointeur
  hôte parqué sur la barre des tâches est tombé sur le chevron des icônes
  masquées : le Chrome plein écran perdu l'activation → page `hidden` → rAF
  gelé → `_waitUntil` ne rend jamais la main. Cible de clic = une petite fenêtre
  topmost `WS_EX_NOACTIVATE` sur l'écran capturé (`click-target.ps1`), qui
  encaisse les clics sans rien activer ; `Page.bringToFront` avant chaque série.

## 7. Ce que les chiffres disent

1. **Le preset est le levier, et il est grand.** Sur ce NVENC (Blackwell), P1
   encode en **2,7 ms** (texte) et **3,1 ms** (jeu) contre 4,7 et 6,5 pour P4 :
   −2 à −3,4 ms de moyenne, −2 à −5,6 ms de p99, à débit égal. Le plan supposait
   « moins d'une milliseconde d'écart entre P1 et P4 » : c'était faux d'un
   facteur trois. P2 est le pire des deux mondes (lent **et** QP de P1) ; P3, P5,
   P6 valent P4 ; P7 coûte 0,2–0,7 ms de plus pour le même QP.
2. **Le prix de P1 en qualité dépend du contenu.** FPS en action : **0** de QP
   (25 vs 25 à 40 Mbit/s, 32 vs 31 à 20), et rien à l'œil sur le flux décodé
   (§6b). Jeu de plateforme : +1 (19 vs 18). Texte qui défile : +5 (31 vs 26),
   au-dessus de la tolérance ; **c'est le seul cas où P1 paie**. Écran fixe :
   identique (la rafale de raffinement converge pareil à QP 8).
3. **Multipass.** Le quart de résolution que le preset ULL active coûte 0,4 ms
   (P4) à 0,6 ms (P1) sur le mouvement, pour le même QP. Mais l'éteindre **casse
   la rafale de raffinement de l'écran fixe** : sans première passe, le rate
   control d'un écran qui vient de s'arrêter n'ose pas dépenser le budget et
   l'image reste à QP 29 au lieu de 8. Ce cas est celui pour lequel §9.1 existe,
   et c'est celui de chaque pause de souris sur un bureau. **Garder le multipass
   quart.** Le tuning LL n'est que ULL avec multipass éteint : même verdict.
4. **AQ spatial** : −5 de QP moyen sur le texte à P1 (25 vs 31), +0,4 ms à P1 /
   +0,3 à P4 ; sur le jeu +1 ms à P1 pour −1 de QP. Le QP moyen sous AQ n'est plus
   tout à fait la même grandeur (l'AQ le redistribue), mais la rafale de
   raffinement converge et le texte gagne. **Candidat sérieux pour compenser P1
   sur le texte**, à trancher à l'œil. AQ temporel : rien, +0,2 ms. À écarter.
5. **VBV.** Le plancher 1/60 s (RateControl.h) tient : sans lui la keyframe sort
   à 32 Ko / QP 50 ; le VBV 1 frame et 2 frames ne changent rien au temps
   d'encode et font des frames plus petites que le budget (22–24 Ko sur 30
   possibles), c'est du débit non dépensé. **Ne rien changer.**
6. **Codec.** À preset égal : AV1 < H.264 < HEVC en temps d'encode (jeu, P4 :
   5,2 / 5,6 / 6,5 ms ; P1 : 3,4 / 3,6 / 3,1). Le décodage client est équivalent.
   La préférence AV1 (licence libre) du moteur est confortée, à condition du
   correctif de masque ci-dessus.
7. **4:4:4** : +0,15 à +0,35 ms. **Intra-refresh** : +0,1 ms, même QP, même
   taille — gratuit. **fps réglé 60 sur écran 165** : frames plus grosses (68 Ko)
   donc encode plus long (+0,9 ms à P4, +0,7 à P1), c'est le budget par frame qui
   change, pas l'encodeur.
8. **AMF (iGPU)** : rien à régler qui compte ; le seul levier est la résolution.
   La pré-analyse est à ne jamais activer en ULL/CBR.

## 8. Recommandation — **appliquée le 04/09/2026** (E1)

Bruno a confirmé après le contenu FPS (§6b) : `kDefaultPreset = 1` dans
`NvencEncoder.cpp`, multipass quart conservé, rien d'autre ne bouge. Vérifié sur
un flux réel sans `MW_NATIVE_TUNING` : la ligne « NVENC ready » dit `P1/ULL
multipass=quarter` sans crochet `[bench]`.

**NVENC : passer de P4 à P1, multipass quart conservé, tout le reste inchangé.**

- FPS en action (la cible) : **−4,3 ms de moyenne et −6 ms de p99** sur l'étape
  encode (7,7 → 3,4 ms), pour **0** de QP et rien de visible sur le flux décodé.
  Plateforme : −3,4 ms pour +1 de QP. C'est le gain le plus grand mesuré sur
  toute la chaîne hôte depuis la phase C — l'étape encode était les deux tiers
  du temps hôte.
- Texte défilant : +5 de QP. Deux façons de le régler, à trancher **à l'œil**
  par Bruno (protocole §5 : A/B en aveugle, netteté du texte, fourmillement en
  mouvement, jank) :
  - **P1 seul** : 2,7 ms, QP 31 sur le texte en mouvement (le texte **fixe** est
    identique à P4 : la rafale converge à QP 8 dans les deux cas — ce qui se lit
    à l'arrêt ne change pas) ;
  - **P1 + AQ spatial** : 3,1 ms, QP 25 sur le texte en mouvement, +1 ms sur le
    jeu (4,1 ms, toujours −2,4 sur P4).
- Ma préférence : **P1 seul**. Le texte qui défile est le seul cas perdant, la
  perte n'est visible que pendant le défilement, et c'est le jeu qui fixe
  l'exigence (§0.1). Si l'A/B montre un fourmillement gênant sur le texte,
  P1 + AQ est la sortie, pour 0,4 ms.

**À ne pas toucher** : multipass (quart), VBV (plancher 1/60 s), tuning ULL,
AQ temporel éteint, lookahead éteint, préférence AV1 > HEVC > H.264.

**AMF** : rien à appliquer, ni sur l'iGPU ni sur la RX 7600 discrète (§8c,
mesurée le 06/09) — le réglage courant (speed, sans pré-analyse) est aussi le
plus rapide sur les deux. **oneVPL** : pas de GPU Intel, la matrice `tu=1..7`
attend.

## 8b. Après la campagne — ce que E4 et E2 ont changé aux chiffres (04/09 après-midi)

- **E4, le budget suit la cadence réelle** (design §9.8) : le même clip FPS à
  60 images/s sous un stream 165, 40 Mbit/s, passe de 29 Ko / QP 25 par image à
  **63 Ko / QP 18** — l'encodeur reçoit enfin le débit que le joueur a autorisé
  au lieu de 60/165 de celui-ci. Encode 3,4 → 3,75 ms pour des images deux fois
  plus grosses. Le fil reste à 38,5 Mbit/s mesurés sur un flux réel.
- **E2, DPB de 4 images pour l'invalidation de référence** (design §9.10) :
  `dpb=1` contre `dpb=4`, deux passes chacun : 3,69 / 3,79 ms contre
  3,75 / 3,77 ms, même taille, même QP. Gratuit.
- Piège de banc rencontré : l'écran virtuel devenu **écran principal** (session
  Parsec de Bruno) a reçu ses fenêtres ; le kiosque passait dessous et le banc
  capturait un bureau à moitié figé (14 Ko / QP 10, faux). `kiosk.ps1` épingle
  désormais la fenêtre TOPMOST sur le rectangle physique de l'écran et attend
  qu'elle existe.

## 8c. AMF sur la RX 7600 discrète (06/09/2026)

La RX 7600 est revenue dans la machine (`gpu=0`), à côté des deux RTX. Elle ne
pilote aucun des écrans du banc, donc mêmes conditions que l'iGPU du 04/09 :
atteinte par le pont inter-GPU depuis Display 3 (VDD sur la RTX), soit une trame
de 14 Mo à travers la mémoire système comptée dans `convert` (2,4 ms mesurés),
la colonne `encode` mesurant l'encodeur seul. Contenu Call of Duty 1440p60, la
même séquence relancée avant chaque passe qu'au §6b. **Ce pilote ne rapporte
toujours aucun QP** (même constat que l'iGPU et que le 02/09) : la qualité AMD
n'a pas de mesure objective ici.

| Réglage (CoD 1440p, 40 Mbit/s sauf mention) | encode ms (moy / p95 / p99) | Ko/frame | cadence |
|---|---|---|---|
| **speed (courant)** | **4,66 / 7,17 / 12,29** | 61,4 | 59,8 fps |
| dpb=1 (pas d'invalidation) | 4,77 / 7,68 / 11,26 | 58,8 | 59,9 |
| balanced | 4,53 / 7,68 / 11,26 | 60,1 | 59,7 |
| quality | 5,04 / 7,17 / 11,26 | 59,8 | 60,0 |
| aq (VBAQ) | 4,68 / 6,66 / 11,26 | 60,0 | 59,9 |
| **pré-analyse** | — | — | **la session meurt** (« stopped producing frames ») |
| VBV 1 frame (29 Ko) | 4,84 / 7,68 / 12,29 | 51,5 | 59,9 |
| VBV 2 frames (59 Ko) | 4,95 / 9,22 / 12,29 | 59,1 | 59,9 |
| H.264 | 4,66 / 7,68 / 11,90 | 59,5 | 59,9 |
| AV1 | 5,38 / 9,22 / 11,26 | 53,0 | 59,8 |
| AV1, dpb=1 | 5,41 / 9,22 / 12,29 | 53,1 | 59,9 |
| intra-refresh | 4,74 / 7,68 / 11,26 | 59,7 | 59,9 |
| fps réglé 60 | 4,88 / 10,24 / 11,26 | 60,2 | 55,5 |
| 20 Mbit/s | 4,79 / 9,22 / 11,26 | 30,3 | 59,9 |
| **1920×1080** | **3,71 / 7,68 / 10,24** | 59,9 | 59,9 |
| défilement de texte 1440p (162 présents/s) | 3,78 / 4,61 / 5,12 | 25,1 | **162,2 fps** |
| défilement · AV1 | 4,51 / 5,63 / 6,14 | 26,7 | 144,3 |
| défilement · 1080p | (voir CSV) | | |

Lecture, et ce qui confirme l'iGPU comme ce qui l'infirme :

1. **La carte discrète tient 1440p60 sans effort et 1080p à pleine cadence.**
   4,66 ms d'encode à 1440p (l'iGPU était à 7,9 sur un clip *plus facile*), 3,71
   à 1080p, et **162 fps** sur le texte défilant 1440p là où l'iGPU plafonnait à
   102. La classe « carte AMD dédiée » n'a pas le problème de cadence de l'iGPU.
2. **Les presets AMF ne bougent presque rien**, exactement comme sur l'iGPU :
   speed 4,66 / balanced 4,53 / quality 5,04 ms. Le « quality » coûte 0,4 ms
   pour aucune mesure de gain (pas de QP). Rien à gagner à quitter « speed ».
3. **La pré-analyse tue encore l'encodeur** en ULL/CBR, sur la carte dédiée comme
   sur l'iGPU — le verrou de `AmfEncoder::init` (§18 du design) est justifié sur
   les deux silicium AMD, pas un hasard de l'iGPU.
4. **AV1 coûte ~0,7 ms de plus que HEVC** (5,38 vs 4,66) ; H.264 = HEVC (4,66).
   Même hiérarchie que NVENC, à ceci près qu'AMF n'a pas l'avance d'AV1 de NVENC.
5. **L'invalidation de référence AMF est gratuite en temps.** `dpb=1` (qui
   l'éteint) contre le défaut : 4,77 vs 4,66 ms HEVC, 5,41 vs 5,38 AV1 — dans le
   bruit, comme le DPB de 4 sur NVENC. Les slots LTR ne coûtent rien à porter.
6. **VBV** : le plancher tient ; 1 ou 2 frames font des images plus petites (51
   au lieu de 61 Ko à VBV 1) sans gagner de temps — du débit non dépensé, même
   verdict que NVENC.

**Recommandation AMF (RX 7600 comme iGPU) : rien à appliquer.** Le réglage par
défaut (usage ultra-low-latency = « speed », pré-analyse interdite, VBAQ au
choix du pilote) est déjà le plus rapide mesuré, et le pilote ne rapporte pas de
QP qui permettrait d'aller chercher un compromis qualité. La seule variable qui
compte sur AMD est la résolution, et elle est le choix de l'utilisateur.

**Le dernier bouton AMD jamais touché : `LowLatencyInternal`** (06/09, au soir).
Le seul réglage AMF que ce moteur n'avait ni posé ni mesuré, et le seul dont
l'en-tête d'AMD annonce « **default = false** » au lieu du « depends on USAGE »
de tous les autres — celui que Sunshine pose explicitement. Il vaut donc une
mesure, pas une supposition. `AmfEncoder::init` le **relit** maintenant, après
avoir posé l'usage et avant toute surcharge, et la réponse tient en un mot :
`lowlatency=1` **déjà**, sur les trois passes par défaut comme sur les forcées.
L'usage ultra-low-latency l'allume lui-même, l'en-tête est trompeur. L'A/B le
confirme, HEVC puis H.264, en alternance, CoD 1440p 40 Mbit/s :

| Passe | encode ms (moy / p95 / p99) | Ko/frame | cadence |
|---|---|---|---|
| HEVC défaut | 4,65 / 9,22 / 11,26 · 4,82 / 9,22 / 12,29 | 59,8 · 59,7 | 59,9 |
| HEVC `lowlatency=1` forcé | 4,93 / 10,24 / 12,29 · 4,83 / 10,24 / 12,29 | 59,6 · 59,9 | 59,9 |
| H.264 défaut | 4,68 / 10,24 / 12,29 · 4,67 / 10,24 / 11,26 | 59,5 · 54,5 | 59,9 |
| H.264 `lowlatency=1` forcé | 4,63 / 9,22 / 11,26 · 4,83 / 10,24 / 12,29 | 60,0 · 57,6 | 59,8 |

Rien à appliquer, et pour la meilleure des raisons : c'était déjà appliqué. Ce
qui reste du travail, c'est la **ligne de log** — l'état effectif du mode est
désormais écrit à côté de `quality` et `preanalysis`, donc un pilote qui
changerait d'avis se verrait au lieu de se deviner. Clé de banc `lowlatency=0|1`
pour rejouer, AV1 excepté (il a son propre `ENCODING_LATENCY_MODE`, déjà au plus
bas).

**Invalidation de référence AMF** — livrée le 06/09 (R4 du plan), mesurée ici
gratuite (point 5) et **configurée sur les trois codecs** : la ligne « AMF
ready » porte « 4 LTR slots every N frames with reference invalidation (reach M
frames) », et `dpb=1` la retire (« no reference invalidation »). Détail du
mécanisme : design §9.10. ⚠️ **Reste à observer sur un vrai lien** : la ligne
« AMF healed frame … from long-term slot bitfield » à la réparation effective —
le banc encode vers un puits (pas de récepteur pour nommer une perte), et le
client Chrome piloté par CDP du banc ne décode pas ce flux (il redemande une
IDR sans jamais monter la vue), donc le test de perte (`mw_drop_test`, qui vit
dans `StreamView`) ne s'arme pas. Même angle mort que l'effet visuel de la
réparation NVENC laissé à l'œil de Bruno au §8b.

## 8d. Intel Quick Sync sur l'UHD Graphics d'un N95 (07/09/2026)

Premier GPU Intel de la flotte (banc `bench-intel`, Intel N95, UHD Graphics 24 EU,
pilote 32.0.101.7088). Le chemin oneVPL n'avait **jamais encodé une frame** avant
ce jour ; il a fallu cinq corrections pour qu'il en encode une, puis pour qu'il
tienne une session. Le détail des cinq est au design §21 — ici, seulement les
chiffres.

**Ce qui a été mesuré.** Bureau fixe (le seul contenu disponible sur ce banc :
pas de clip, 5 Go de libre sur le disque), 1 passe de 8 s par ligne, 20 Mbit/s,
intra-refresh actif, capture DXGI de l'écran 2560×1440 de la machine.

| Codec | TU | Taille | fps | encode moy / p95 / p99 (ms) | convert moy | Ko/frame |
|---|---|---|---|---|---|---|
| HEVC | 1 | 1920×1080 | 59,6 | 13,33 / 18,43 / 24,58 | 1,15 | 36,4 |
| HEVC | 4 | 1920×1080 | 59,8 | 12,59 / 18,43 / 20,48 | 1,37 | 36,4 |
| HEVC | 7 | 1920×1080 | 59,7 | **11,46** / 15,36 / 18,43 | 1,64 | 36,4 |
| H.264 | 1 | 1920×1080 | 58,1 | 15,53 / 20,48 / 26,62 | 0,74 | 39,1 |
| H.264 | 4 | 1920×1080 | 59,8 | 12,61 / 16,38 / 22,53 | 1,49 | 36,5 |
| H.264 | 7 | 1920×1080 | 59,7 | 13,38 / 18,43 / 18,43 | 1,11 | 36,5 |
| HEVC | 7 | 2560×1440 | 59,6 | 13,43 / 18,43 / 20,48 | 1,17 | 36,4 |
| HEVC | 1 | 2560×1440 | 58,3 | 16,47 / 20,48 / 26,62 | 0,43 | 36,4 |

**Verdict : rien à appliquer, et pour la troisième fois.** Le `TargetUsage`
d'Intel se comporte comme les presets d'AMD : 1,9 ms d'écart entre le bout
qualité et le bout vitesse, du même ordre que la dispersion entre deux passes du
même réglage, et le défaut du moteur (TU7) est déjà le bord rapide. Le débit par
frame ne bouge pas d'un octet (36,4 Ko partout) : en CBR sur un bureau fixe, le
contenu ne discrimine rien.

⚠️ **Aucune mesure objective de qualité sur ce banc non plus.** Le pilote Intel ne
rapporte pas de QP moyen, exactement comme AMD (§8c). Un jugement de qualité
Intel resterait un A/B à l'œil.

**Le seul réglage qui a compté ne se règle pas — il se pose.** `LowPower`
(le moteur à fonction fixe, VDENC) : HEVC 1080p60 est passé de **16,3 à 10,5 ms**
et le 1440p de **21,4 à 13,4 ms**. Il est désormais demandé à chaque session,
avec repli automatique sur le moteur général si la génération ne l'a pas.

**Ce que ça vaut, honnêtement.** Un N95 est un SoC à 4 cœurs sans SMT et 24 EU :
ces chiffres disent que le chemin **tient 60 fps en 1080p et en 1440p** sur le
plus petit Intel qui existe, pas ce que vaut un Arc ou un Core de bureau. Et la
capture, la conversion, l'encodage, le décodage du navigateur et la page
tournaient tous sur la même machine.

**Flux réel.** Chrome 152 sur la machine elle-même, hôte natif Intel :
HEVC `hvc1.1.144.L123.B0`, `descLen=111` (VPS/SPS/PPS extraits de la keyframe
servie), première image décodée 1920×1080 NV12 en matériel, 65,7 s de session,
**1689 présents tous portés**, audio 13 142 paquets / 0 jeté, zéro erreur de
décodeur, arrêt propre. Le gouverneur de lien descend le débit à chaque montée
de délai (20000 → 4196 kbps) et **toutes** ses baisses sont appliquées — c'était
l'objet de la correction du HRD.

**Puis depuis une autre machine, ce qui est la vraie mesure.** Chrome sur bench-desk
→ hôte Intel, en LAN, appairage par PIN, `webrtc-dc-udp` : **latence affichée
11,4 à 14,2 ms**, deux sessions de 289 s et 320 s, 2327 puis 2558 présents **tous
portés**, 4831 frames émises, encode 7,05 / 11,26 / 14,34 ms, total hôte
8,84 / 13,31 / 18,43 ms, 57 800 paquets audio / 0 jeté, **63 événements d'entrée
injectés** (souris et clavier), arrêt propre. Les 41 à 55 ms du loopback étaient
ceux d'un N95 qui encodait, décodait et servait la page à la fois — à ignorer.

Et c'est là seulement que le gouverneur a pu **remonter** : 16000 → 20000 kbps en
cinq paliers, toutes les hausses appliquées. En loopback la machine était saturée
et il ne faisait que descendre, donc la moitié montante du correctif de `Reset`
n'y était pas prouvée.

⚠️ **Le banc était injoignable en LAN, et ce n'était pas le pare-feu tiers.** La
boîte Windows « autoriser cette application ? » s'était ouverte dans la session
console sans que personne ne la voie, et Windows en avait fait deux règles de
**blocage** pour le binaire — un blocage l'emporte sur toute règle de port, donc
les autorisations ajoutées à la main ne servaient à rien. SSH marchait pendant ce
temps et masquait le problème.

⚠️ Deux limites propres à Intel, mesurées ici : le débit ne peut pas **monter**
au-dessus de celui de l'init (`Reset` refuse), donc la moitié montante du budget
par cadence réelle (E4) est plafonnée ; et il n'y a **pas d'invalidation de
référence** sur oneVPL (`NumRefFrame = 1`), donc une perte se répare par
keyframe.

## 8e. Intel : la matrice de paramètres sur du vrai contenu (07/09/2026)

Le §8d mesurait sur un bureau fixe et concluait que le `TargetUsage` ne se voit
pas. ⚠️ **C'était vrai du bureau fixe et faux du reste** : sur le clip Call of
Duty, TU1 coûte **trois fois** le temps d'encodage de TU7. Le défaut du moteur ne
change pas — il était déjà au bon bout de l'échelle — mais le raisonnement, si.

**Protocole.** Clip CoD 1440p60 relancé en kiosque plein écran sur l'écran
capturé avant **chaque** passe (mêmes secondes du même métrage), stream HEVC
1080p60, 20 Mbit/s, intra-refresh, 10 s par passe, banc `--native-bench` vers un
puits.

⚠️ **Ce banc est saturé, et il faut le dire avant les chiffres.** L'N95 décode le
clip 1440p60 *et* encode 1080p60 sur les mêmes 24 EU. Quatre passes du réglage
par défaut donnent 51,4 / 60,9 / 70,2 / 77,8 ms — **±20 % de dispersion**. Rien
en dessous d'un facteur ~1,5 n'est mesurable ici.

| Réglage | fps | encode moy / p95 / p99 (ms) | Ko/frame |
|---|---|---|---|
| **(défaut)** | 16,8 | **51,40** / 81,92 / 147,46 | 67,3 |
| `extbrc=1` | 17,1 | 49,40 / 90,11 / 134,49 | 64,8 |
| `dpb=1` | 14,4 | 51,72 / 98,30 / 294,91 | 63,5 |
| `gaming=1` | 13,5 | 54,44 / 106,50 / 196,61 | 62,1 |
| `vbv=1` | 14,5 | 55,40 / 114,69 / 196,61 | 66,2 |
| `tu=4` | 14,8 | 57,16 / 114,69 / 183,39 | 65,6 |
| `mbbrc=1` | 13,1 | 60,65 / 106,50 / 360,45 | 67,8 |
| `vbv=2` | 12,0 | 67,26 / 122,88 / 524,29 | 65,4 |
| `lowdelaybrc=1` | 9,3 | 71,94 / 196,61 / 648,94 | 66,6 |
| `mbbrc=0` | 11,1 | 72,02 / 212,99 / 360,45 | 65,9 |
| `winbrc=60` | 11,4 | 72,36 / 180,22 / 267,47 | 66,7 |
| `tu=1` | 6,1 | **141,35** / 245,76 / 965,98 | 69,2 |
| `lowpower=0` | 4,5 | **185,90** / 393,22 / 633,88 | 65,3 |

**Deux réglages sortent du bruit, et ce sont les deux que le moteur pose déjà.**

- `lowpower=0` — le moteur à shaders au lieu du bloc fixe : **3,6× plus lent**,
  4,5 fps. VDENC n'est pas une optimisation, c'est la condition d'existence du
  chemin Intel.
- `tu=1` — le bout « qualité » du TargetUsage : **2,8× plus lent**, 6 fps, et le
  débit par image ne bouge pas (69,2 Ko contre 67,3). On paie tout le temps pour
  rien de visible. TU7, le défaut, est le bon.

**Tout le reste est dans la dispersion du défaut lui-même** (49 à 72 ms, contre
51–78 pour quatre passes identiques). `extbrc=1` est nominalement le meilleur,
mais l'écart est plus petit que le bruit : rien à appliquer. Verdict identique à
NVENC après la campagne du 04/09 et à AMF au §8c — **le moteur était déjà réglé
juste**, et cette fois on sait aussi *pourquoi* : les deux seuls leviers qui
comptent sont ceux qu'il pose.

⚠️ `winbrc=60` a fait mourir une passe entière (`still executing`) avant que le
plafond d'attente ne soit relevé : la fenêtre glissante coûte assez cher pour
qu'une image dépasse la seconde sur ce matériel.

⚠️ **Le clic→photon n'a pas pu être mesuré sur ce banc.** Le drapeau
click-to-photon exige un build debug (`LatencyFlag` est gaté sur `QT_DEBUG`) ;
un vrai build Debug est dix fois trop lent ici (notre passe de conversion passe
de 0,4 à 10,6 ms), donc un arbre Release portant seulement `QT_DEBUG` a été bâti
pour la mesure. L'hôte confirme chaque clic (« [LatencyFlag] injected click at
1711,1056 ») mais la sonde du navigateur ne voit **jamais** le drapeau dans
l'image décodée — ni sur le clip, ni sur un bureau fixe où le pipeline est sain.
La contre-vérification par capture d'écran sur le banc ne tranche pas : `BitBlt`
ne voit pas une fenêtre *layered*, donc « rien vu » n'y prouve rien. Deux bugs
réels ont été trouvés en montant cette mesure (§21.8), mais **le chiffre lui-même
n'est pas acquis**, et il ne faut pas en inventer un.

## 8f. Ce que coûte le VBV sur Intel, et pourquoi la marge a été retirée (07/09/2026)

Le §21.6 du design explique comment le budget par image *peut* monter sur Intel :
en dimensionnant le tampon de bitstream à l'init, parce que c'est ce que `Reset`
valide. Reste à savoir ce que ce tampon coûte, puisqu'il est aussi le VBV.

**Mesure.** Texte défilant plein écran (mouvement sur toute l'image, à chaque
frame — le pire cas d'un flux de bureau), HEVC 1080p60, 20 Mbit/s,
intra-refresh. `vbv=<n>` demande exactement n images au débit du flux ; sans clé,
la règle du moteur.

| Réglage | VBV | Ko/frame moy | p95 | p99 | occupation du lien (p95) |
|---|---|---|---|---|---|
| `vbv=1` | 85 Ko | 40,6 | 60,0 | 64,0 | **24 ms** |
| `vbv=2` | 85 Ko | 40,8 | 64,0 | 66,2 | 26 ms |
| `vbv=3` | 125 Ko | 41,9 | 88,0 | 96,0 | 35 ms |
| `vbv=6` | 250 Ko | 56,2 | 112,0 | 156,5 | **45 ms** |
| marge ×2 (250 Ko) | 250 Ko | 55,4 | 104,0 | 154,6 | **42 ms** |

**Lecture.** La marge fait exactement ce qu'elle promet — 40,6 → 55,4 Ko par
image, soit **+37 % de bits pour le même débit sur le fil** quand l'écran bouge
moins vite que le flux. Et elle le fait payer là où ça compte : le pic par image
passe de 60-64 Ko à 104-155 Ko, c'est-à-dire de **~26 ms à ~42 ms** d'occupation
du lien pour une seule image à 20 Mbit/s.

**Décision de Bruno, appliquée** : « qualité légèrement moindre sur écran fixe
acceptable ; aucune augmentation volontaire de la latence pour gagner en
netteté ». `kBudgetHeadroom = 1` : le VBV revient à la règle partagée (85 Ko,
une image au débit du flux), le pic redescend à 64 Ko au p99, et le budget par
image ne monte pas. Une marge ×3 aurait demandé 375 Ko, soit ~150 ms de lien
pour une image — jamais envisagée.

⚠️ Ce qui est perdu est la moitié **montante** de E4 sur Intel seulement : une
image lente n'y dépense pas les bits que ses frames auraient valus. La moitié
descendante — celle qui compte quand un lien souffre — fonctionne exactement
comme ailleurs, et surtout **les demandes sont maintenant plafonnées au lieu
d'être refusées** : avant le 07/09 un `Reset` refusé laissait le débit là où il
était, warning à l'appui.

⚠️ La voie qui donnerait les deux — gros tampon pour autoriser la hausse,
`MaxFrameSizeP` pour borner l'image — **n'existe qu'en VBR** (« used in VBR based
bitrate control modes and ignored in others »). Elle demanderait de changer le
mode de contrôle de débit de ce chemin ; à instruire séparément si le sujet
revient.


## 8g. Où la marge de VBV se voit vraiment : la transition, et le jeu (07/09/2026)

Le §8f donne le coût moyen. Il ne dit pas **quand** il se paie, ce qui est la
seule chose qui compte pour un joueur. Deux contenus, deux moments :

- **`wake.html`** : une page parfaitement immobile qui se met à défiler à un
  instant connu. C'est le moment où l'on attrape une fenêtre — l'encodeur ronronne
  sur une image fixe et on lui demande brutalement une image difficile.
- **le clip Call of Duty** : mouvement soutenu, sans transition.

Chaque passe est rejouée avec `vbv=1` (85 Ko, ce qui est livré) et `vbv=6`
(250 Ko, la marge de ce matin). HEVC 1080p60, 20 Mbit/s, intra-refresh.
`link ms` = temps qu'une image met à passer sur un lien exactement au débit
réglé, calculé depuis les octets du CSV.

### La transition, découpée

| Phase | Réglage | images | Ko moy | **Ko max** | **link ms max** |
|---|---|---|---|---|---|
| écran fixe | `vbv=1` | 17 | 38,2 | **40,7** | **16,7** |
| écran fixe | `vbv=6` | 15 | 40,7 | **135,2** | **55,4** |
| 30 premières images du mouvement | `vbv=1` | 30 | 32,6 | 59,3 | **24,3** |
| 30 premières images du mouvement | `vbv=6` | 30 | 38,1 | 148,8 | **60,9** |
| mouvement soutenu ensuite | `vbv=1` | 185 | 40,3 | 64,9 | **26,6** |
| mouvement soutenu ensuite | `vbv=6` | 185 | 41,2 | 160,1 | **65,6** |

### Ce que ça dit, moment par moment

**Écran fixe : la marge sert la rafale de raffinement, et rien d'autre.** Avec
85 Ko chaque passe est plafonnée à ~41 Ko, avec 250 Ko une passe peut mettre
135 Ko d'un coup. C'est exactement la netteté d'un bureau immobile — et c'est ce
qui a été accepté comme perte : la rafale converge toujours, en plus de passes.
Le « 55 ms de lien » de cette ligne ne se ressent pas, personne ne bouge.

**Le moment où l'on attrape une fenêtre : +17 % de bits contre une image à
61 ms.** La marge donne des premières images un peu plus fines (38,1 contre
32,6 Ko de moyenne), et fait passer la pire de 24 à **61 ms de lien** — près de
quatre intervalles de trame à 60 fps. Dans ce moteur, ce qui arrive pendant
qu'une image occupe le lien n'attend pas : l'émetteur ne garde qu'un delta
(C5), donc les images produites entre-temps sont **remplacées**. On échange donc
une image plus nette contre un à-coup et deux ou trois images sautées, au moment
précis où l'on regarde *où* la fenêtre est arrivée, pas si son texte est net.

**En jeu : quasiment aucun gain, tout le coût.** En mouvement soutenu la moyenne
est la **même** (40,3 contre 41,2 Ko) — le CBR tient la moyenne, et E4 ne dilate
plus le budget puisque les images arrivent à la cadence du flux. Ce qui change
est uniquement la queue : la pire image passe de 64,9 à 160,1 Ko, soit **27 →
66 ms de lien**. Un jeu produit ces images difficiles en continu (explosions,
caméra qui balaie), donc le pic se répète — et chaque fois c'est une image en
retard ou une image sautée, au pire moment.

### La distinction qui explique tout

Ce que l'on a appelé « la marge » ce matin était **deux choses** :

- **le plafond de budget** (`kBudgetHeadroom`) : ce que l'encodeur vise quand
  les images arrivent moins vite que le flux. C'est lui qui donne les +37 % de
  bits du §8f, et il ne sert que sur un contenu qui bouge **lentement** ;
- **le tampon** (le VBV) : de combien une image isolée peut dépasser la moyenne.
  C'est lui qui coûte les pics ci-dessus.

Sur Intel ils sont indissociables : `Reset` n'accepte de relever le plafond que
si le tampon a été dimensionné pour. NVENC et AMF acceptent la hausse sans
toucher au tampon — c'est pourquoi eux gardent le gain de E4 gratuitement, et
c'est une différence de vendeur, pas de conception.

⚠️ **Limite du banc** : l'N95 ne tient qu'une vingtaine d'images par seconde sur
ce contenu, donc « mouvement soutenu à pleine cadence » n'est pas vraiment
éprouvé. Sur une machine qui tient 60 fps, E4 ne dilaterait pas du tout le
budget en jeu — le gain de la marge y serait encore plus proche de zéro, et le
coût le même.


## 8h. L'étage de repli : Media Foundation et OpenH264 (07/09/2026)

Design §22. Toutes les passes H.264 4:2:0, CBR, VBV d'une image ; « encode »
comprend la relecture GPU→CPU là où il y en a une.

| Machine | Chemin | Contenu | Résolution / débit | encode moy. / p99 (ms) | Ko/img |
|---|---|---|---|---|---|
| bench-desk (9900X) | AMF (référence GPU) | bureau fixe | 1080p60 · 20 Mbit/s | 2,83 / 3,34 | 9,6 |
| bench-desk | MF logiciel `H264 Encoder MFT` (relecture CPU) | bureau fixe | 1080p60 · 20 Mbit/s | 10,5 / 15,4 | 7,0 |
| bench-desk | OpenH264 (relecture CPU, 4 threads) | bureau fixe | 1080p60 · 20 Mbit/s | 10,7 / — | 9,0 |
| bench-desk | OpenH264, image synthétique (test) | dégradé mouvant | 1080p60 · 20 Mbit/s | 3,3 / — | 33 |
| **bench-arm** (Snapdragon 7c) | **`QCOM Hardware Encoder - H264`** (MF matériel, textures D3D11) | bureau | 720p60 · 8 Mbit/s | **12,5 / 16,8** | 8,6 |
| bench-arm | `QCOM Hardware Encoder - HEVC`, flux réel | bureau | 1440p (suragrandi) | 20,9 / 24,6 | — |
| **bench-vm** (4 vCPU 9900X) | **KMS scruté → mmap → CPU → OpenH264**, flux réel | bureau XFCE | 1080p · 20 Mbit/s | **5,4 / 18,4** | — |

Flux navigateur réels (Chrome sur bench-desk, par le rendez-vous) : MF logiciel
HEVC `hev1.1.6.L153.B0` 12,1 ms ; OpenH264 `avc1.42c033` 16,3 ms ; Snapdragon
HEVC 29,5 ms ; VM Debian `avc1.42c02a` **8,1 ms**. Tous décodés en matériel par
le client.

Ce que ça dit : sur le Snapdragon, le transform Qualcomm est **le seul encodeur
matériel** qu'un logiciel de stream y ait jamais utilisé (Sunshine y est en x264
logiciel) ; à 1440p suragrandi il coûtait 21 ms, d'où la règle « jamais de
suragrandissement sur le repli ». Sur la VM, 4 cœurs Zen 5 encodent le 1080p en
5 ms de moyenne — la voie CPU n'est pas une punition sur un CPU moderne ; sur
l'N95 (§8d) elle le serait, et c'est là qu'un plafond automatique reste à
mesurer — §8i le mesure.

## 8i. Quand le CPU ne suit pas : ce que baisser la résolution rapporte (08/09/2026)

Le seul banc où la voie CPU souffre vraiment : l'**N95** (4 cœurs Alder Lake-N,
pas de SMT), encodeur OpenH264 4 threads, contenu réel (le clip Call of Duty en
kiosque, comme §8e), H.264 CBR 20 Mbit/s, 10 s par passe.

| Résolution | encode moy. / p95 / p99 (ms) | Cadence atteinte | Ko/img |
|---|---|---|---|
| **1920×1080** | **39,71 / 73,73 / 81,92** | **24,7 fps** | 57,5 |
| 1280×720 | 33,54 / 53,25 / 131,07 | 29,1 fps | — |
| **960×540** | **19,78 / 36,86 / 40,96** | **48,8 fps** | 35,1 |

Trois choses que ces chiffres disent, et une qu'ils ne disent pas :

1. **Baisser la résolution rapporte, mais pas proportionnellement.** De 1080p à
   540p il y a **4× moins de pixels** et seulement **2× moins de temps** : une
   part fixe du coût (les threads par tranches, le contrôle de débit, la
   recherche de mouvement par macrobloc) ne suit pas la surface. Le palier
   intermédiaire est le pire marché : 720p ne gagne que **16 %** sur 1080p.
2. **Aucune résolution n'atteint 60 fps.** Même à 540p l'encodage coûte 19,8 ms
   quand l'intervalle en vaut 16,7. Un plafond de résolution ne rend donc pas
   cette machine capable de 60 fps ; il déplace le plafond de cadence de **25 à
   49 fps**.
3. **Le p99 de 720p (131 ms) est une anomalie**, pas une tendance — une image
   isolée trois fois plus chère que la moyenne, là où 1080p et 540p ont un p99
   proche du double de leur moyenne. À reproduire avant d'en conclure quoi que
   ce soit.

Ce qu'ils ne disent pas : **lequel des deux plafonds un joueur préfère.** Pacer
à la cadence que la machine tient (25 fps en 1080p net) et baisser la résolution
pour gagner de la fluidité (49 fps en 540p flou) sont deux réponses également
défendables, et le choix est une question de perception, pas de mesure — c'est
la règle 0.2.2 : les chiffres, une recommandation, puis confirmation.

⚠️ La passe 1080p **à 30 fps forcés** n'a rien rendu (le banc sort sans écrire
ses statistiques, sans plantage) — non élucidé, et sans conséquence pour la
lecture ci-dessus : le temps d'encodage d'une image ne dépend pas de la cadence
à laquelle on les demande.

## 8j. Le banc de shaders de réduction : `mw-scaler-bench` (17/09/2026)

Quand le stream est plus petit que l'écran (1440p → 1080p), le host réduit
l'image dans la passe de conversion avec **un seul fetch bilinéaire, sans
mipmap, en espace gamma** (`ColorConvert.cpp:463`, `GlConvert.cpp:329`) — voir
§28 du design. Pour choisir mieux, un banc à part : `mw-scaler-bench`, dans
`backend/native-host/tools/scaler-bench/`, cible CMake optionnelle
(`-DMW_BUILD_TOOLS=ON`, Windows seulement, jamais installée).

Ce qu'il mesure, sur **chaque GPU de la machine** et pour chaque cas
(4K → 1440p/1080p/720p, 1440p → 1080p/720p), en SDR et en HDR :

- **le temps de la passe seule**, par timestamps GPU (`D3D11_QUERY_TIMESTAMP`
  sous un `DISJOINT` par lot de 50, lot jeté si l'horloge a bougé), 300 runs
  par candidat en tourniquet, moyenne tronquée 10 %/10 %, médiane, p95, min.
  Exception : `VideoProcessorBlt` part sur une file que les timestamps 3D ne
  voient pas (0 µs sur NVIDIA et Intel) — pour lui, chrono mur de l'appel à
  l'événement de complétion, étiqueté « wall clock », **pas comparable** ;
- **la qualité**, contre une référence CPU (Lanczos-3 en lumière linéaire,
  noyau dilaté au ratio, double précision) : PSNR/SSIM sur le signal encodé
  (sRGB, ou codes PQ en HDR) ; et le **test de défilement** : la source
  bouge d'un pixel, la différence temporelle du candidat est projetée sur
  celle de la référence → *gain de mouvement* (1 = idéal, moins = flou) et
  *flicker* (énergie que le mouvement n'explique pas = aliasing, ce que
  l'encodeur paie sans rien montrer ; 0 = idéal) ;
- des **crops** (bords les plus chargés, texture, centre) avec loupe
  synchronisée dans le rapport.

Candidats : bilinéaire (= le host), bilinéaire + mipmaps, Catmull-Rom et
Mitchell en 9 fetches (rayon fixe) et dilatés (vrais passe-bas), Lanczos-2/3
fixes et dilatés — chacun en perceptuel et en linéaire (SRV `_SRGB`), fp32 et
`min16float` ; FSR 1.0 EASU (± RCAS), NIS, SGSR1 tels que livrés par leur SDK
(`tools/scaler-bench/third_party/`, MIT/BSD) ; et `ID3D11VideoProcessor` en
trois réglages. Trois vérités du banc à connaître avant de lire :

- **NIS refuse** : `NVScalerUpdateConfig` n'accepte que 0,5..1 (upscale) et
  sa tuile en mémoire partagée est dimensionnée pour ça — rapporté
  « unsupported », jamais exécuté hors contrat. FSR1 et SGSR1, eux, tournent
  en réduction, et le rapport montre ce que ça donne (mal : ce sont des
  upscalers sans passe-bas).
- **FSR1 en fp16 sur NVIDIA** sort une image fausse (PSNR 20 dB contre 27 en
  fp32 ; AMD et Intel donnent la même image dans les deux) : le chemin
  `min16float` de fxc ne tient pas les astuces de paquetage half du header.
  Le rapport le signale de lui-même dès qu'un fp16 diverge de son jumeau
  fp32 de plus d'un dB.
- Le fp16 n'est jamais plus rapide sur ces trois GPU de bureau — les
  « fp16 honoured » du tableau des GPU disent que le pilote l'accepte, pas
  qu'il en tire quelque chose.

Source 1440p : `C:\Test\00_Background.png` (une mire de texte). Source 4K :
`C:\Test\01_4K.png`, capture 4K de 0 A.D. (CC BY-SA 3.0, Wikimedia Commons,
provenance dans `01_4K.txt` à côté), UI de jeu avec petit texte. Le HDR est
synthétisé depuis le PNG : blanc SDR à 1.0, hautes lumières poussées à 10.0
sur les 2 % les plus clairs, quatre pastilles à 4/8/12 et une grille 1 px.

```
cmake -S backend -B build -DMW_BUILD_TOOLS=ON …
build\backend\native-host\tools\scaler-bench\mw-scaler-bench.exe
    [--cases 1440p-1080p] [--adapters 0,2] [--variants bilinear,catmullrom9]
    [--iterations 300] [--no-hdr] [--out C:\Test\scaler-bench\<date>]
```

Sortie : `results.json`, `crops/`, et `report.html` autonome (filtres GPU /
cas / gamme / espace / précision, barres temps sur axe log, barres qualité au
choix de la métrique, nuage qualité × temps, galerie, tableau + CSV). Les
shaders sont lus à côté de l'exe (`scaler-bench/shaders/`) : un filtre se
retouche et se relance sans rebuild.

### Les chiffres (campagne du 17/09/2026, `C:\Test\scaler-bench\campaign-1`)

RTX 5060 Ti, SDR, temps de la passe seule (noyau carré une passe, tel que
mesuré ; l'intégration est séparable, donc moins chère) :

| Cas | bilinéaire (gamma) | bilinéaire linéaire | Lanczos-2 dilaté linéaire |
|---|---|---|---|
| 1440p → 1080p | 16 µs · flicker 0,159 · gain 0,78 · 28,7 dB | 16 µs · 0,086 · 0,91 · 33,3 dB | 261 µs · 0,017 · 0,91 · 38,5 dB |
| 1440p → 720p | 7 µs · 0,663 · 0,82 · 26,1 dB | 7 µs · 1,225 · 0,95 · 26,9 dB | 190 µs · 0,046 · 0,91 · 39,1 dB |
| 4K → 1440p | 88 µs · 0,150 · 0,86 · 32,5 dB | 88 µs · 0,126 · 0,86 · 33,2 dB | 486 µs · 0,015 · 0,89 · 39,4 dB |
| 4K → 1080p | 74 µs · 0,238 · 0,90 · 32,1 dB | 73 µs · 0,212 · 0,91 · 33,1 dB | 445 µs · 0,020 · 0,88 · 39,4 dB |
| 4K → 720p | 39 µs · 4,56 · 1,04 · 22,9 dB | 36 µs · 4,56 · 1,04 · 22,9 dB | 408 µs · 0,031 · 0,87 · 39,1 dB |

Même filtre sur l'Arc A380 : 480 µs à 1080p ; sur l'iGPU Radeon 2 CU du Ryzen
7000 : 3,5 ms (son bilinéaire fait déjà 300 µs — pas un GPU représentatif d'un
780M à 12 CU). Les filtres à rayon fixe (Catmull-Rom/Mitchell 9 fetches,
Lanczos fixes, FSR1, SGSR1) sont nets mais aliasés à 1,33 et s'effondrent à
ratio 2 (flicker 1,5–1,6). Lanczos-3 dilaté = la référence (60 dB) pour 2× le
prix. VideoProcessor : NVIDIA = bilinéaire, Intel un peu mieux (30,8 dB),
AMD plus flou. Sur le 780M (UM790Pro, Mesa), la passe intégrée — séparable —
coûte 0,57 ms par image à 1080p → 720p contre 0,45 ms en bilinéaire. Décision
et intégration : §28.3 du design.

## 8k. Priorités sous charge 3D : GPU REALTIME, CPU HIGH (23/09/2026)

Question : sous un jeu qui sature le GPU de l'encodeur, que rapporte de monter
la priorité du worker, maintenant qu'il peut tourner en SYSTEM (ou élevé) et
tenir `SeIncreaseBasePriorityPrivilege` ? Interrupteurs de banc, défaut
inchangé (classe GPU HIGH depuis `92f6b122`) : `MW_GPU_PRIORITY=realtime`
(active le privilège, demande la classe REALTIME, repli HIGH loggé) et
`MW_CPU_PRIORITY=high` (classe CPU HIGH, jamais REALTIME). Le worker loggue
aussi l'état HAGS de chaque GPU qu'il utilise.

Montage : DualRTX, instance `--dev` lancée **élevée** (jeton `elevated`, pas
de service SYSTEM installé sur ce banc ; le privilège est le même), harnais
`scripts/bench/acceptance`, 1080p60 HEVC figé, client Chrome sur l'iGPU AMD
(jamais le GPU de l'encodeur), `mw-gpu-load --topmost` à niveau **figé** sur
le GPU de l'encodeur. 4 passes par configuration sous charge (mesure 10 s
chacune, 6 s d'installation), moyennes des passes ; p99 = celui de l'overlay.
HAGS : **activé** dans Windows ; le pilote de la RTX le gère (`Enabled:True`),
ceux de l'Arc et de l'iGPU AMD ne le gèrent pas (`AlwaysOff`, dxdiag).

**NVENC, RTX 5060 Ti (HAGS activé)** — écran virtuel rendu par la RTX
(`MW_VDD_GPU`), page animée, charge niveau 500 (21,4 ms GPU/image, ~47 fps) :

| | latence | hôte | encodage moy. / p99 | récupération p99 | images/s | fps de la charge |
|---|---|---|---|---|---|---|
| sans charge (HIGH) | 5,2 | 2,9 | 2,8 / 5,3 | 0,1 | 60 | — |
| sans charge, REALTIME | 3,8 | 2,4 | 2,2 / 9,8 | 0,2 | 60 | — |
| base (HIGH) | 32,5 | 31,3 | 29,5 / 51,7 | 19,9 | 31,5 | 46,7 |
| A — GPU REALTIME | **20,2** | **18,6** | **18,2 / 21,3** | 0,5 | **43,8** | 46,5 |
| B — CPU HIGH | 32,1 | 31,1 | 27,7 / 48,4 | 25,3 | 32,5 | 46,8 |
| A + B | **19,5** | **18,5** | **17,8 / 21,0** | 0,4 | **44,0** | 46,6 |

**oneVPL, Arc A380 (pilote sans HAGS)** — écran 1, charge niveau 90
(16,5 ms GPU/image, ~58 fps) :

| | latence | hôte | encodage moy. / p99 | récupération p99 | images/s | fps de la charge |
|---|---|---|---|---|---|---|
| sans charge (HIGH) | 6,9 | 5,9 | 5,4 / 15,9 | 25,0 | 59 | — |
| base (HIGH) | 21,5 | 19,2 | 16,7 / 32,5 | 20,7 | 44,2 | 58,7 |
| A — GPU REALTIME | 21,7 | 18,3 | 16,0 / 25,1 | 29,2 | 43,0 | 58,4 |
| B — CPU HIGH | 22,3 | 19,8 | 16,7 / 32,4 | 21,3 | 44,8 | 58,6 |
| A + B | 21,4 | 18,4 | 15,6 / 21,6 | 16,8 | 43,0 | 58,3 |

Lecture :

- **REALTIME est le levier, sur NVENC** : l'encodage attendait derrière le
  jeu (29,5 ms, p99 52) ; en REALTIME il passe devant — 12 ms de latence de
  moins, 12 images/s de plus, p99 d'encodage divisé par 2,4 — et le jeu
  garde sa cadence (46,7 → 46,5 fps, même temps GPU). Sans charge, rien ne
  se dégrade.
- **Sur l'Arc, presque rien** : ~1 ms d'hôte et le p99 d'encodage de 32 à
  ~19-25 ms (une passe sur quatre garde un pic), latence moyenne inchangée.
  Le pilote de l'Arc ne fait pas d'ordonnancement matériel ; pourquoi la
  classe y compte moins n'est pas établi.
- **CPU HIGH ne change rien** (charge GPU seule ; un jeu gourmand en CPU
  pourrait différer, non mesuré).
- Les ~18 ms d'encodage qui restent sur NVENC en REALTIME (2,8 sans
  charge) ne sont pas expliqués : hypothèse, le temps de rendre la main à
  une image du jeu déjà lancée, puisqu'elle est proche de son temps GPU.
- Au banc, la première passe après chaque redémarrage de l'instance échoue
  (« no picture after 60s ») : écartée, relancée.

Pas encore de défaut changé : REALTIME ne se demande qu'avec
`MW_GPU_PRIORITY=realtime`, et seulement là où le jeton tient le privilège
(worker SYSTEM ou élevé).

## 8l. Les ~18 ms qui restent, et le vrai jeu (23/09/2026)

Même montage que §8k (DualRTX, instance `--dev` élevée, écran virtuel rendu
par la RTX, NVENC, 1080p60 HEVC, client sur l'iGPU AMD, HAGS activé).

**Balayage de la charge** — `mw-gpu-load` à trois niveaux figés, 3 passes
par case :

| niveau | GPU/image de la charge | classe | latence | hôte | encodage moy. / p99 | images/s |
|---|---|---|---|---|---|---|
| 230 | 10,6 ms | HIGH | 18,5 | 17,6 | 15,4 / 26,9 | 49,7 |
| 230 | 10,6 ms | REALTIME | 9,3 | 8,5 | **8,1** / 10,9 | 54,7 |
| 500 | 21,4 ms | HIGH | 32,0 | 30,7 | 27,6 / 41,8 | 34,0 |
| 500 | 21,4 ms | REALTIME | 19,8 | 18,6 | **18,1** / 21,2 | 43,7 |
| 900 | 36 ms | HIGH | 35,6 | 34,7 | 34,5 / 48,2 | 27,0 |
| 900 | 36 ms | REALTIME | 35,2 | 34,3 | **33,9** / 35,6 | 27,0 |

En REALTIME, l'encodage vaut 0,76, 0,85 puis 0,95 fois le temps GPU d'une
image de la charge : le travail du stream passe devant la file du jeu, mais
attend la fin de l'image en cours. L'outil dessine chaque image en un seul
appel plein écran de 10 à 36 ms que le GPU ne coupe pas, et la capture est
calée sur ses fins d'image (DWM compose derrière elle) : on arrive juste
après le début d'une image et on l'attend presque entière. À 36 ms, la
cadence du stream se cale même sur celle de la charge (27 = 27).

**Vrai jeu : Resident Evil Requiem**, réglages au maximum (ray tracing
Haut, textures, maillages, ombres, SSS et AO hauts, brouillard volumétrique
maximal, DLSS Qualité max ; génération d'images et Reflex coupés, SDR,
fenêtré 1920×1080 sur l'écran virtuel). Scène d'ouverture (rue sous la
pluie), rejouée par « Continuer » à chaque configuration. **RTX à 99-100 %
de 3D** (re9 seul : 95-96 %). Alterné base, REALTIME, base, REALTIME ;
4 lectures de l'overlay à 10 s d'écart par tour (8 par classe) :

| | latence | hôte | acquisition | conversion | encodage moy. / p99 | images/s |
|---|---|---|---|---|---|---|
| HIGH (défaut) | 4,3 | 3,2 | 0,11 | 0,14 | 2,65 / 10,3-16,4 | 61 |
| REALTIME | 3,8 | 2,4 | 0,10 | 0,13 | **1,94 / 2,9-3,3** | 61 |

Lecture :

- **Les ~18 ms de §8k sont un artefact de l'outil de charge**, pas une
  limite de l'encodeur : un jeu découpe son image en centaines d'appels
  courts, et le GPU donne la main entre deux. Sous RE9 à 100 %, la classe
  HIGH tient déjà 3,2 ms d'hôte et 61 images/s.
- **REALTIME reste utile en vrai jeu** : −0,8 ms d'hôte et surtout un p99
  d'encodage divisé par 4 (10-16 → 3 ms), c'est-à-dire moins d'à-coups.
- Rien de visible côté bureau pendant les tours REALTIME (DWM stable à 3 %
  de la RTX, stream à 61 images/s) ; pas de mesure de fluidité du jeu
  lui-même (pas de PresentMon sur le banc).
- `mw-gpu-load` exagère donc l'attente : ses chiffres absolus sous charge
  sont un pire cas, pas ce qu'un joueur verra.

Pièges rencontrés :

- RE9 choisit son GPU seul, et prenait l'Arc (l'écran M27Q y est branché
  depuis le 22/09) malgré `[Render/Adapter]` et `TargetDisplay`. Seule la
  préférence graphique Windows par application, au format
  `SpecificAdapter=10DE&2D04&53511462;GpuPreference=1073741824;` (hexa :
  vendeur, appareil, sous-système), le met sur la RTX. Posée pour le banc,
  retirée après.
- RE9 **plante** (0xc0000409) quand l'écran virtuel sur lequel il tourne
  disparaît, ici à chaque redémarrage de l'instance. Le jeu est donc relancé
  à chaque configuration, stream déjà ouvert.
- La « première passe après redémarrage » de §8k ne venait pas de l'hôte :
  aucun lancement ne lui arrivait. Le harnais cherchait la fenêtre « streamer
  ce PC » une seule fois, 1,5 s après le clic ; elle met parfois bien plus
  (10,5 s mesurées une fois), les requêtes de la page restant bloquées
  derrière les listes d'apps des hôtes éteints (504 à 5 s) tant que Chrome
  n'a que 6 connexions par origine. Harnais corrigé (`5d77fb46`) : 10
  redémarrages, 10 premiers lancements réussis.

**Défaut changé (23/09/2026)** : la classe GPU REALTIME est désormais
demandée par défaut quand le jeton du worker le permet (worker SYSTEM par le
service, ou élevé), HIGH sinon ou en cas de refus.
`MW_GPU_PRIORITY=high` garde l'ancien comportement pour un « avant ».

## 8m. Deux horloges : images répétées ou sautées à fréquence égale (23/09/2026)

Question : quand le stream tourne à la fréquence de l'écran du client, la
dérive entre l'horloge de l'hôte et celle du client fait-elle répéter ou
sauter des images ? Aujourd'hui rien ne cale la phase : `CadenceAlign` ne
cale que le débit.

**Montage.**
- DualRTX, instance `--dev` ; écran virtuel rendu par la RTX (NVENC), 1080p
  HEVC ; `scroll.html` sur l'écran capturé, une image neuve par
  rafraîchissement.
- Client Chrome sur un autre GPU : à 120 Hz sur l'écran de l'Arc, à 60 Hz sur
  celui de l'iGPU AMD. Aucun changement de mode d'écran.
- Mesure par `scripts/bench/cadence` : sonde injectée par CDP, rien dans
  l'app. Elle horodate chaque rafraîchissement (rAF), chaque image remise au
  décodeur et chaque image dessinée.
- Passes de 150 s. Une répétition = un rafraîchissement sans image neuve ; un
  saut = une image jamais montrée. Chaque événement est attribué à l'hôte
  (image jamais envoyée), au client (rafraîchissement sauté par Chrome) ou à
  la phase (image tombée de l'autre côté d'une frontière).

| cas | client / hôte (Hz) | battement | répétées + sautées / min (phase) | trous de l'hôte / min | latence |
|---|---|---|---|---|---|
| 120, tearing, dérive (20 s) | 120,000 / 119,976 | 42 s | 234 + 234 | 12 | 3,6 ms |
| 120, tearing, calé | 120,000 / 120,000 | — | 46 + 46 | 1 | 3,0 ms |
| 120, vsync, calé ×2 | 120,000 / 120,000 | — | 3 + 2 ; 0,4 + 0,4 | 3-4 | 15,5-15,9 ms |
| 60, tearing, dérive ×2 | 60,000 / 59,988-59,990 | 82-99 s | 70 + 70 ; 38 + 38 | 77-89 | 4,9-5,1 ms |
| 60, vsync, dérive ×2 | 60,000 / 59,989-59,991 | 94-114 s | **0 + 0** ; **0 + 0** | 32-51 | **26-29 ms** |

Lecture :

- **La dérive existe et se voit.** L'écran virtuel tourne à 59,988-59,991 Hz
  pour 60 demandés, et à 119,976 Hz à 120 quand il ne se cale pas.
  - Le glissement mesuré sur les arrivées vaut celui que prédisent les deux
    fréquences (0,0097 cycle/s pour 0,010-0,012 Hz d'écart).
  - En tearing, c'est-à-dire le défaut de Chrome sur ordinateur, les couples
    « répétée + sautée » se regroupent dans une fenêtre de chaque battement :
    3 à 10 par seconde pendant 20 à 40 % du temps, rien ailleurs. Les
    arrivées sont alors près de la frontière : 0,8-0,9 de la période à
    120 Hz, 0,4-0,9 à 60 Hz, où le décodage étale le dessin.
- **Même calé, le tearing en garde une partie** (46/min). Ce n'est pas la
  dérive : la durée de décodage varie (p99 ~10 ms à 120 Hz) et fait passer le
  dessin de part et d'autre d'une frontière.
- **Le mode vsync ne répète rien, même en dérive.** Il ne peut sauter qu'en
  gardant une image de réserve (Chromium, tearing off), et cette réserve
  absorbe le passage de la frontière. Le prix est la latence : 15,5 ms au
  lieu de 3 à 120 Hz, 26-29 ms au lieu de 5 à 60 Hz. L'image attend le
  rafraîchissement suivant, puis un de plus.
  - Safari, Firefox et les mobiles affichent sans cette réserve (freshest au
    rAF). Ils doivent donc subir la dérive comme le tearing. Non mesuré.
- Les « trous de l'hôte » à 60 Hz sont un artefact de ce banc.
  - Toutes les ~40 s, la phase saute en arrière de ~0,4 période ; l'hôte perd
    6 à 8 images en quelques secondes et le Chrome client saute autant de
    rafraîchissements au même moment.
  - C'est le même DWM des deux côtés.
- **Limite du banc.**
  - Sous Windows, le rAF de Chrome suit l'horloge de composition, celle de
    l'écran principal, et pas celle de son écran : un client sur l'écran AMD
    à 60 Hz battait à 119,98 Hz.
  - Sur un seul PC, l'hôte et le client ne sont donc jamais deux horloges
    tout à fait indépendantes : à 120 Hz, l'écran virtuel s'est calé sur le
    client dans 3 passes sur 4.
  - Une passe tearing 120 est écartée : la page de contenu n'a produit que
    ~104 images/s.
  - La dérive entre deux vrais écrans reste à mesurer avec un second PC comme
    client.

**Verdict.**

Un calage de phase par l'hôte ne vaut pas le chantier.
- L'hôte ne peut pas déplacer les présentations d'un jeu ni l'horloge d'un
  écran physique, seulement retarder la capture, ce qui ajoute de la latence.
- Pour l'écran virtuel, régler son mode sur la fréquence mesurée du client
  (fréquence fractionnaire) allongerait le battement. À vérifier : il tourne
  déjà à 59,988 pour « 60 ».

Le levier est côté client : **une réserve décidée par la phase**. Le client
connaît sa grille de rafraîchissement et la phase d'arrivée de chaque image.
- Chemins vsync :
  - garder l'image de réserve seulement quand les arrivées approchent la
    frontière ;
  - sinon présenter au rafraîchissement suivant ;
  - gain attendu ~1 période de latence la plupart du temps (−8 ms à 120 Hz,
    −16 ms à 60 Hz), à fluidité égale ;
  - Safari et iOS gagneraient une réserve quand il en faut une.
- Tearing : retarder de quelques ms une image qui tomberait juste avant la
  frontière, seulement dans la fenêtre de dérive. Cela suppose de connaître
  le décalage du compositeur. Plus incertain.

## 8n. Pipeline vidéo D3D12, deuxième essai (26/09/2026 →)

Le chemin D3D11 passe derrière une interface (`WindowsVideoPipeline`) avant
qu'un chemin D3D12 ne vienne à côté (design §32). Cette section garde la
référence D3D11 et les portes du chantier.

### 8n.0 La référence D3D11 et la porte G0 (26/09/2026)

**Montage.**
- DualRTX : RTX 5060 Ti (HAGS actif), Arc A380 et iGPU AMD. Les pilotes de
  ces deux derniers ne gèrent pas HAGS.
- HEVC 1080p60 à 20 Mb/s, `--native-bench` de 12 s par passe. Contenu :
  `scroll.html` dans un Chrome dédié, en kiosque sur l'écran capturé.
- Classe GPU **HIGH** partout : l'agent tourne avec un jeton limité, et
  REALTIME ne lui est pas accessible. Les deux binaires sont comparés dans la
  même classe.
- Charge : `mw-gpu-load` niveau 500 sur la RTX (~20 ms de GPU par image,
  ~48 i/s), à la place de RE9, dont le menu demande un clic.
- Binaires figés dans `bench-out\d3d12v2` : `ref-bin` (moteur de `main`
  5d9cf81e) et `g0-bin` (`c65789ff`, le chemin D3D11 derrière l'interface).

**Référence (C0.4)**, deux passes par cas :

| cas | hôte moy. / p50 / p99 (ms) | encodage moy. / p99 | conversion | i/s |
|---|---|---|---|---|
| RTX 1080p60 | 2,19 / 2,12 / 3,18 | 1,93 / 2,33 | 0,12 | 59,9 |
| RTX sous charge 500 | 49,8 / 42,0 / 79,4 | 34,5 / 42,1 | 0,11 | 27,4 |
| Arc 1080p60 | 8,27 / 5,72 / 27,9 | 7,46 / 24,4 | 0,17 | 57,2 |
| Arc 1440p120 | 11,4 / 7,38 / 41,0 | 9,54 / 35,1 | 0,15 | 86,6 |
| iGPU AMD 1080p60 | 8,40 / 7,98 / 17,2 | 8,21 / 17,0 | 0,10 | 59,9 |

**G0 (C0.7).** `scripts/bench/ab-native-bench.ps1`, tours alternés (A puis
B, puis B puis A). Critères du plan : |Δ moyenne `host_total`| ≤ 0,2 ms,
Δ p99 ≤ +10 %, images/s à 1 %, octets/image et QP à 3 %.

| cas | tours | hôte moy. réf → G0 (ms) | p99 réf → G0 | i/s | octets/image (Ko) | verdict |
|---|---|---|---|---|---|---|
| RTX 1080p60 | 4 | 2,095 → 2,100 | 3,17 → 3,18 | 60 / 60 | 31,3 / 31,3 | tenu |
| RTX sous charge 500 | 4 | 66,6 → 67,0 | 83,1 → 83,2 | 27,1 / 27,0 | 52,6 / 53,4 | seuil de 0,2 ms hors d'échelle |
| Arc 1080p60 | 4 | 15,0 → 13,1 | 41,4 → 40,8 | 52,5 / 53,6 | 36,1 / 35,4 | non tenu : un tour de la réf. à 21,6 |
| Arc 1080p60, confirmation | 8 | 14,06 → 13,90 | 41,9 → 41,6 | 52,9 / 52,9 | 35,8 / 36,0 | **tenu** |
| iGPU AMD 1080p60 | 4 | 8,53 → 8,76 | 17,5 → 17,7 | 59,9 / 59,9 | 33,9 / 34,0 | non tenu : +0,23 ms |
| iGPU AMD, confirmation | 8 | 8,66 → 8,56 | 17,8 → 17,5 | 59,9 / 59,9 | 34,2 / 34,2 | **tenu** |
| pont RTX → Arc | 4 | 9,56 → 9,57 | 14,1 → 15,6 | 59,5 / 59,5 | 34,3 / 34,4 | non tenu : p99 +10,5 % |
| pont RTX → Arc, confirmation | 8 | 9,68 → 9,93 | 15,7 → 18,0 | 59,4 / 59,2 | 34,3 / 34,3 | non tenu : une passe G0 où l'encodeur de l'Arc cale |

Lecture :
- La conversion ne bouge pas : 0,10 à 0,21 ms des deux côtés, à 0,01 ms
  près, dans tous les cas. Le refactor ne change rien au travail GPU.
- Les échecs des premiers passages viennent du bruit, pas du code.
  - L'Arc varie de 12 à 14 ms d'un tour à l'autre avec le même binaire, et
    un tour de la référence est monté à 21,6 ms. Le matin, la même mesure
    donnait 8,3 ms.
  - L'iGPU AMD est bimodal : 8,3 ou 8,75 ms selon le tour.
  - Un p99 sur 12 s ne repose que sur les 7 pires images.
- Les confirmations passent à 8 tours et mettent le binaire G0 en tête, pour
  croiser l'effet d'ordre. Sur l'Arc et l'iGPU AMD, elles tiennent tous les
  critères.
- Le pont reste « non tenu » à la lettre, à cause d'une seule passe : au
  tour 5, l'encodeur de l'Arc cale côté G0 (encodage p99 31,6 ms,
  56,9 i/s).
  - Sans cette passe, les deux binaires sont à 9,66 contre 9,65 ms, et à
    15,5 contre 15,6 ms de p99.
  - Sur les médianes des 8 tours, G0 est même devant : 9,69 contre 9,72 ms,
    p99 15,2 contre 15,8.
  - La conversion, qui contient la copie du pont, ne bouge pas : p99 de 1,7
    à 1,9 ms des deux côtés. Le calage est dans oneVPL, que le refactor ne
    touche pas.
- Sous charge 500, l'hôte attend le draw non préemptible de `mw-gpu-load`
  (~20 ms), avec un p50 bimodal (62 ou 76 ms). Un seuil absolu de 0,2 ms n'y
  a pas de sens. Les deux binaires y sont à 0,6 % en moyenne, 0,1 % en p99
  et 0,3 % en images/s.

**Verdict.** G0 est tenu au banc, en classe HIGH, sur les deux cas que le
plan exige (RTX chargée, Arc au repos), ainsi que sur l'iGPU AMD. Le pont
ne l'est qu'en médiane, à une passe près ; l'écart est dans l'encodeur de
l'Arc, pas dans le code déplacé. Reste le test manuel de Bruno sur la build
de la branche. Deux écarts au plan : REALTIME (jeton élevé) et RE9 n'ont pas
été joués, `mw-gpu-load` a remplacé le jeu.

### 8n.1 Les sondes de la phase 1, jeton limité (26/09/2026)

**Pourquoi une interop D3D11 → D3D12, et pas un pipeline purement D3D12.**
C'est une contrainte de Windows, pas un choix. Les deux API de capture
d'écran ne livrent leurs images qu'à un device D3D11 : Desktop Duplication
(`DuplicateOutput` refuse un device D3D12) et Windows.Graphics.Capture
(`IDirect3DDevice` bâti sur D3D11). Il n'existe pas de capture du bureau
native D3D12. Le pipeline fait donc un seul saut, le plus tôt possible :
- le device D3D11 ne fait plus qu'acquérir et relâcher l'image, et signaler
  ou attendre deux fences partagées ;
- la surface capturée est ouverte en D3D12 par handle NT, mis en cache ;
- conversion et encodage sont en D3D12, sans copie.

La poignée de main coûte 8 à 18 µs au repos et 0,2 à 0,3 ms sous charge
(tableau C). Écartés : le hook du `Present` D3D12 des jeux (intrusif,
anti-triche), la capture par un pilote d'affichage indirect (surfaces D3D11
elles aussi, et un pilote signé à livrer), D3D11On12 (une couche de
traduction, pas un chemin plus direct).

**Montage.**
- DualRTX : RTX 5060 Ti (HAGS actif), Arc A380, iGPU AMD.
- `mw-d3d12-lab` lancé par `scripts/bench/d3d12-lab-campaign.ps1`
  (`50df96fb`, `bbfb897e`), sorties dans `bench-out\d3d12v2\g1b`.
- Charge : `mw-gpu-load` calé à ~45 images/s sur le GPU testé (~22 ms de GPU
  par image, la saturation d'un jeu). Une passe de charge par variante, et
  chaque sonde bornée dans le temps.
- Jeton limité : classe GPU **HIGH** pour la conversion (tableau A) ;
  `GLOBAL_REALTIME` refusé (`0x887A002B`).
- ⚠️ **Corrigé le 27/09** : les sondes `encode`, `vendors` et `interop` ne
  prenaient pas la classe du produit. Elles tournaient en classe **NORMAL**,
  avec leurs files en HIGH (`d600bbea`). Les tableaux B et C sont donc en
  NORMAL ; ils sont rejoués en HIGH et en REALTIME au §8n.2.
- Une première passe (`g1`) est écartée : des variantes y tournaient hors de
  la fenêtre de 60 s de la charge (repérables à l'absence de `loadFps`).

**A. La conversion** — 1440p → 1080p, Lanczos-2, pointeur, 120 soumissions/s.
Temps mur moyen / p99, en ms.

| GPU | file | repos | sous charge | dont GPU sous charge |
|---|---|---|---|---|
| RTX | D3D11 | 0,62 / 2,31 | 20,84 / 23,22 | 0,21 / 0,22 |
| RTX | PS DIRECT HIGH | 0,50 / 2,57 | 22,19 / 22,89 | 0,20 / 0,21 |
| RTX | PS DIRECT NORMAL | 0,51 / 2,05 | 22,19 / 22,82 | 0,20 / 0,21 |
| RTX | CS COMPUTE HIGH | 0,57 / 2,03 | 22,39 / 22,89 | 0,27 / 0,44 |
| Arc | D3D11 | 2,27 / 3,06 | 22,98 / 24,79 | 1,32 / 3,10 |
| Arc | PS DIRECT HIGH | 2,18 / 2,27 | 22,69 / 24,34 | 1,25 / 2,83 |
| Arc | PS DIRECT NORMAL | 2,18 / 2,47 | 22,69 / 23,47 | 1,11 / 1,33 |
| Arc | CS COMPUTE HIGH | 1,58 / 1,88 | 34,95 / 69,73 | 26,15 / 69,41 |
| iGPU AMD | D3D11 | 6,96 / 7,67 | 35,69 / 63,48 | 14,92 / 42,57 |
| iGPU AMD | PS DIRECT HIGH | 6,92 / 7,67 | 38,75 / 69,33 | 15,83 / 45,89 |
| iGPU AMD | PS DIRECT NORMAL | 6,91 / 7,61 | 35,45 / 61,91 | 14,94 / 39,19 |
| iGPU AMD | CS COMPUTE HIGH | 6,46 / 7,78 | 27,08 / 33,01 | 26,93 / 32,81 |

**B. Les encodeurs** — HEVC 1080p60 CBR 20 Mb/s, temps mur par image P
(moyenne / p99, ms).

| GPU | encodeur | repos | sous charge |
|---|---|---|---|
| RTX | D3D12 VE | 9,97 / 17,25 | 44,89 / 45,40 |
| RTX | D3D12 VE après la conversion | 9,43 / 15,83 | 66,48 / 67,59 |
| RTX | NVENC-D3D12 | 3,22 / 6,67 | 22,39 / 22,76 |
| Arc | D3D12 VE | 6,09 / 29,25 | 4,46 / 21,82 |
| Arc | D3D12 VE après la conversion | 7,39 / 26,64 | 34,07 / 88,60 |
| iGPU AMD | D3D12 VE | 9,03 / 9,40 | 9,06 / 9,61 |
| iGPU AMD | D3D12 VE après la conversion | 16,88 / 17,50 | 33,35 / 62,13 |
| iGPU AMD | AMF-DX12 | 5,32 / 5,87 | 22,70 / 23,86 |

**C. La poignée de main DDA** — `interop`, bande codée de `scroll.html` sur
l'écran du GPU. Tenue de l'image (moyenne / p99, ms), et lectures fausses sous
charge.

| GPU | `ddasync` | repos | sous charge | lectures fausses sous charge |
|---|---|---|---|---|
| RTX | none | 0,25 / 1,09 | 0,15 / 0,29 | 0 sur 516 |
| RTX | gpu | 0,28 / 0,96 | 0,22 / 0,42 | 0 sur 267 |
| RTX | cpu | 0,68 / 1,10 | 42,36 / 64,87 | 0 sur 268 |
| Arc | none | 0,24 / 0,44 | 0,26 / 0,40 | 99 sur 457 |
| Arc | gpu | 0,27 / 0,49 | 0,33 / 0,48 | 0 sur 286 |
| Arc | cpu | 2,21 / 3,03 | 23,11 / 47,27 | 0 sur 246 |
| iGPU AMD | none | 0,17 / 0,30 | 0,19 / 0,43 | 0 sur 551 |
| iGPU AMD | gpu | 0,22 / 0,41 | 0,25 / 0,52 | 0 sur 432 |
| iGPU AMD | cpu | 4,26 / 5,54 | 25,98 / 42,54 | 0 sur 430 |

**Lecture.**
- **Au repos, la conversion D3D12 vaut la D3D11**, et la bat un peu : PS
  DIRECT à 0,50 / 2,18 / 6,92 ms contre 0,62 / 2,27 / 6,96 (RTX / Arc /
  AMD). Le compute est plus rapide sur l'Arc et l'AMD (1,58 et 6,46 ms).
- **Sous charge, en classe HIGH, tout attend l'image du jeu.** Le travail GPU
  de la conversion ne change pas (0,2 ms sur la RTX, 1,1 à 1,3 ms sur l'Arc),
  mais le temps mur monte à ~22 ms, une image de `mw-gpu-load`, en D3D11
  comme en D3D12. NORMAL ou HIGH, `CreatorID` propre ou non : aucune
  différence. Dans cette classe, la priorité de file ne sert à rien contre une
  charge saturante.
- **Le compute sous charge dépend du GPU** :
  - RTX : égal au PS (22,4 ms) ;
  - Arc : bien pire (35 ms, p99 70 ms), le moteur compute n'obtient pas sa
    part ;
  - iGPU AMD : meilleur que le PS (27 ms, p99 33, contre 35-39 et 62-69).
    C'est le seul cas où le détour par COMPUTE (C3.4) aurait un intérêt.
- **Les encodeurs sous charge** :
  - D3D12 VE sur l'Arc et l'AMD ne voit pas la charge (4,5 et 9,1 ms ; sur
    l'Arc, plus vite qu'au repos, les horloges restant hautes) ;
  - D3D12 VE sur la RTX passe à 45 ms (deux images du jeu), contre 22 ms
    pour NVENC-D3D12 : la route D3D12 de la RTX reste NVENC-D3D12 (phase 7),
    comme au repos ;
  - sur l'AMD, AMF-DX12 gagne au repos (5,3 contre 9,0 ms) et perd sous
    charge (22,7 contre 9,1) : il attend l'image du jeu, VE non. G4
    tranchera ;
  - VE après la conversion hérite de l'attente de la conversion (33 à 66 ms).
- **La poignée de main DDA sous charge** :
  - `ddasync=gpu` garde l'image 0,2 à 0,3 ms côté CPU et ne lit rien de
    faux ;
  - sans synchro, l'Arc lit faux 99 images sur 457 (22 %) ;
  - `cpu` bloque le fil de capture le temps d'une image du jeu (23 à 42 ms).
    La synchro GPU est donc obligatoire, et gratuite.
- **La cadence de capture sous charge** : avec la poignée de main, la RTX ne
  capture plus que ~22 images/s (267 en 12 s), contre ~43 sans. DWM ne
  réécrit l'image qu'après la lecture D3D12, qui attend l'image du jeu. Le
  chemin D3D11 obéit au même ordre (keyed mutex) : la référence D3D11 sous
  charge tenait 27 i/s (§8n.0). La limite est la classe de priorité, pas
  l'interop.

**Verdict partiel (G1).** En classe HIGH, D3D12 ne change rien sous une
charge saturante : conversion et capture attendent l'image du jeu comme en
D3D11. Ce qui est acquis quelle que soit la classe :
- la poignée de main `ddasync=gpu` ;
- la conversion en PS sur DIRECT (égale à D3D11), le compute (C3.4)
  seulement pour l'iGPU AMD, à confirmer ;
- les encodeurs : NVENC-D3D12 pour la RTX, D3D12 VE pour l'Arc (insensible
  à la charge, mais il lui faut le contrôle de débit maison, phase 6),
  D3D12 VE ou AMF-DX12 pour l'AMD (G4).

La décision sur les files attend la passe au jeton élevé (classe REALTIME,
files `GLOBAL_REALTIME`) et RE9 : voir §8n.2.

### 8n.2 Les sondes de la phase 1 en REALTIME, et sous RE9 (27/09/2026)

**Montage.**
- Mêmes GPU, même campagne, binaires figés dans
  `bench-out\d3d12v2\g1-bin`, sorties dans `bench-out\d3d12v2\g1c`.
- Deux corrections du labo d'abord :
  - `d600bbea` : les sondes `encode`, `vendors` et `interop` prennent la
    classe GPU comme le produit (REALTIME si le jeton le permet, HIGH
    sinon), et leurs files la suivent (`GLOBAL_REALTIME` en REALTIME) ;
  - `232825ac` : une lecture D3D12 pas finie n'est plus comparée. La sonde
    attendait la fence B 2 s sans vérifier, puis relisait le tampon de
    l'image d'avant, compté « périmé ».
- **Jeton élevé (REALTIME)** : un exécuteur lancé une fois par UAC, qui ne
  lance que la campagne, avec des paramètres vérifiés. **Jeton limité
  (HIGH)** : la même campagne, depuis la session.
- **RE9**, la copie propre (`Resident Evil Requiem - Copy`) : scène
  d'ouverture sous la pluie, réglages de la copie (ray tracing haut, qualité
  « Highest », 2048×1152 natif sans upscaling, fenêtré). Le GPU est choisi
  par la préférence graphique Windows (RTX, puis Arc), remise à l'identique
  après ; `config.ini` est restauré, empreinte vérifiée. Fenêtre du jeu sur
  l'écran de l'Arc (DISPLAY1), rendue par la RTX pour sa passe, par l'Arc
  pour la sienne.
- Occupation 3D relevée chaque seconde : RTX 98-99 %, Arc 97-99 %, y compris
  quand la page de test de l'interop a le focus ou recouvre le jeu.

**D. La conversion sous RE9** — celle du tableau A. Temps mur moyen / p99,
en ms.

| GPU | file | REALTIME | HIGH |
|---|---|---|---|
| RTX | D3D11 | 0,48 / 0,92 | 2,59 / 16,41 |
| RTX | PS DIRECT GLOBAL_REALTIME | 0,46 / 0,83 | refusé |
| RTX | PS DIRECT HIGH | 0,47 / 0,87 | 2,55 / 16,39 |
| RTX | PS DIRECT NORMAL | 0,47 / 0,87 | 2,65 / 16,55 |
| RTX | CS COMPUTE GLOBAL_REALTIME | 0,49 / 0,86 | refusé |
| RTX | CS COMPUTE HIGH | 0,48 / 0,84 | 2,87 / 16,47 |
| Arc | D3D11 | 1,97 / 5,75 | 20,20 / 60,40 |
| Arc | PS DIRECT GLOBAL_REALTIME | 1,64 / 3,54 | refusé |
| Arc | PS DIRECT HIGH | 1,84 / 7,02 | 22,81 / 72,30 |
| Arc | PS DIRECT NORMAL | 1,77 / 4,82 | 22,88 / 72,09 |
| Arc | CS COMPUTE GLOBAL_REALTIME | 20,70 / 70,02 | refusé |
| Arc | CS COMPUTE HIGH | 22,80 / 77,67 | 88,13 / 340,98 |

**E. Les encodeurs sous RE9** — HEVC 1080p60 CBR 20 Mb/s, temps mur par
image P (moyenne / p99, ms).

| GPU | encodeur | REALTIME | HIGH |
|---|---|---|---|
| RTX | D3D12 VE | 4,75 / 5,25 | 7,56 / 32,51 |
| RTX | D3D12 VE après la conversion | 5,20 / 5,63 | 9,48 / 34,93 |
| RTX | NVENC-D3D12 | 1,76 / 2,43 | 3,36 / 16,78 |
| Arc | D3D12 VE | 3,71 / 8,69 | 3,67 / 8,16 |
| Arc | D3D12 VE après la conversion | 4,51 / 9,07 | 30,37 / 94,55 |

**F. La poignée de main DDA sous RE9** — tenue de l'image (moyenne / p99,
ms), lectures fausses, images capturées par seconde.

| GPU | `ddasync` | REALTIME | fausses | i/s | HIGH | fausses | i/s |
|---|---|---|---|---|---|---|---|
| RTX | none | 0,22 / 0,34 | 1100 sur 1285 | 107 | 0,23 / 0,37 | 466 sur 1038 | 86 |
| RTX | gpu | 0,27 / 0,46 | 0 sur 1159 | 97 | 0,26 / 0,43 | 0 sur 1020 | 85 |
| RTX | cpu | 1,48 / 2,64 | 0 sur 1158 | 96 | 2,97 / 16,81 | 0 sur 1037 | 86 |
| Arc | none | 0,24 / 0,47 | 1202 sur 1344 | 112 | 1,71 / 11,48 | 0 sur 8 | 0,7 |
| Arc | gpu | 0,27 / 0,66 | 0 sur 1328 | 111 | 0,73 / 7,38 | 0 sur 19 | 1,6 |
| Arc | cpu | 2,64 / 10,44 | 0 sur 1307 | 109 | 94,16 / 1016,22 | 0 sur 17 | 1,2 |

En HIGH sur l'Arc, le mode `cpu` atteint son délai d'une seconde une fois
(relâchement avant la lecture). Avant `232825ac`, la même passe comptait 5
lectures « périmées » sur 10 en `gpu` : l'artefact décrit plus haut.

**G. La conversion sous `mw-gpu-load`** — temps mur moyen / p99, en ms. La
ligne « PS DIRECT » est en `GLOBAL_REALTIME` en REALTIME, en HIGH en jeton
limité (qui refuse `GLOBAL_REALTIME`) ; de même pour « CS COMPUTE ».

| GPU | file | repos, REALTIME | charge, REALTIME | charge, HIGH |
|---|---|---|---|---|
| RTX | D3D11 | 0,54 / 1,08 | 7,68 / 22,22 | 18,04 / 22,76 |
| RTX | PS DIRECT | 0,51 / 1,03 | 9,42 / 22,32 | 21,40 / 22,82 |
| RTX | CS COMPUTE | 0,58 / 1,07 | 9,19 / 22,31 | 20,33 / 22,93 |
| Arc | D3D11 | 2,29 / 2,65 | 9,40 / 16,40 | 19,80 / 45,30 |
| Arc | PS DIRECT | 1,22 / 1,55 | 9,32 / 16,37 | 20,42 / 45,49 |
| Arc | CS COMPUTE | 1,58 / 1,66 | 35,74 / 70,75 | 32,08 / 61,42 |
| iGPU AMD | D3D11 | 6,97 / 7,75 | 12,65 / 37,56 | 36,69 / 63,88 |
| iGPU AMD | PS DIRECT (HIGH : `GLOBAL_REALTIME` refusé) | 6,94 / 7,73 | 15,54 / 37,65 | 35,48 / 62,32 |
| iGPU AMD | CS COMPUTE | 6,50 / 7,87 | 11,28 / 23,56 | 27,44 / 31,18 |

**H. Les encodeurs sous `mw-gpu-load`** — temps mur par image P (moyenne /
p99, ms).

| GPU | encodeur | repos, REALTIME | charge, REALTIME | charge, HIGH |
|---|---|---|---|---|
| RTX | D3D12 VE | 9,66 / 16,60 | 39,59 / 44,73 | 44,80 / 66,62 |
| RTX | D3D12 VE après la conversion | 8,38 / 11,27 | 31,99 / 45,17 | 62,55 / 67,79 |
| RTX | NVENC-D3D12 | 2,70 / 6,26 | 22,29 / 22,69 | 22,35 / 22,83 |
| Arc | D3D12 VE | 6,02 / 29,10 | 4,35 / 23,36 | 4,15 / 21,22 |
| Arc | D3D12 VE après la conversion | 4,55 / 21,84 | 24,56 / 45,92 | 31,83 / 90,23 |
| iGPU AMD | D3D12 VE | 9,03 / 9,41 | 9,03 / 9,42 | 9,03 / 9,46 |
| iGPU AMD | D3D12 VE après la conversion | 16,93 / 17,75 | 29,19 / 43,65 | 39,99 / 71,54 |
| iGPU AMD | AMF-DX12 | 5,30 / 5,81 | 21,91 / 24,28 | 21,12 / 22,92 |

**I. La poignée de main sous `mw-gpu-load`** — tenue (moyenne / p99, ms),
lectures fausses, images capturées par seconde.

| GPU | `ddasync` | REALTIME | fausses | i/s | HIGH | fausses | i/s |
|---|---|---|---|---|---|---|---|
| RTX | gpu | 0,18 / 0,33 | 0 sur 499 | 41,5 | 0,20 / 0,35 | 0 sur 266 | 22,1 |
| RTX | none | 0,16 / 0,30 | 514 sur 515 | 42,9 | 0,16 / 0,27 | 0 sur 519 | 43,2 |
| RTX | cpu | 18,81 / 38,20 | 0 sur 507 | 42,2 | 42,28 / 62,37 | 0 sur 267 | 22,2 |
| Arc | gpu | 0,29 / 0,43 | 0 sur 487 | 40,6 | 0,30 / 0,49 | 0 sur 352 | 29,3 |
| Arc | none | 0,28 / 0,41 | 480 sur 482 | 40,2 | 0,26 / 0,43 | 112 sur 486 | 40,4 |
| Arc | cpu | 20,09 / 23,65 | 0 sur 479 | 39,9 | 21,76 / 42,66 | 0 sur 266 | 22,2 |
| iGPU AMD | gpu | 0,22 / 0,49 | 0 sur 774 | 64,5 | 0,23 / 0,57 | 0 sur 456 | 38,0 |
| iGPU AMD | none | 0,18 / 0,42 | 777 sur 822 | 68,4 | 0,19 / 0,44 | 0 sur 560 | 46,6 |
| iGPU AMD | cpu | 13,59 / 34,88 | 0 sur 765 | 63,7 | 24,80 / 40,29 | 0 sur 452 | 37,7 |

Au repos, sans poignée de main et en REALTIME, les lectures fausses sont
déjà 658 sur 1439 (RTX), 1136 sur 1424 (Arc), 1420 sur 1439 (iGPU AMD).

**Lecture.**
- **C'est la classe REALTIME qui compte, pas l'API.** Sous RE9, la
  conversion ne voit plus le jeu : 0,46 à 0,49 ms sur la RTX (p99 < 0,93),
  1,6 à 2,0 ms sur l'Arc (p99 3,5 à 5,8). En HIGH, elle attend l'image du
  jeu : p99 de 16 ms sur la RTX, de 60 à 72 ms sur l'Arc. D3D11 et D3D12
  suivent le même ordre dans chaque classe.
- **Sur l'Arc, qui n'a pas HAGS, HIGH s'effondre sous un vrai jeu** :
  conversion à 20-23 ms de moyenne, capture de la sonde d'interop à 1-2
  images/s, lectures qui attendent plus d'une seconde. Le produit n'est pas
  dans ce cas : son worker obtient REALTIME (jeton élevé, journaux de la
  prod du 26/09). Seul un worker sans élévation le serait.
- **La meilleure file de conversion en REALTIME est PS sur DIRECT en
  `GLOBAL_REALTIME`** : 0,83 ms de p99 contre 0,92 pour D3D11 sous RE9 sur la
  RTX, et 3,54 contre 5,75 sur l'Arc. Au repos sur l'Arc, elle gagne encore
  plus d'une milliseconde (1,22 contre 2,29).
- **Le compute ne vaut rien sur l'Arc** : p99 de 70 ms, sous RE9 comme sous
  charge synthétique. Sur la RTX, il égale le PS.
- **iGPU AMD** : le pilote refuse `GLOBAL_REALTIME` sur les files DIRECT et
  VIDEO_ENCODE (`0x887A0004`, la file retombe en HIGH) mais l'accepte sur
  COMPUTE. Sous charge synthétique, le compute en `GLOBAL_REALTIME` est la
  meilleure variante : 11,3 / 23,6 contre 12,7 / 37,6 pour D3D11. RE9 ne
  tourne pas sur cet iGPU (plan, §9-4).
- **`mw-gpu-load` reste un pire cas** : même en REALTIME, ses images d'un
  seul dessin non préemptible font attendre la conversion jusqu'à ~22 ms au
  p99 sur la RTX, là où RE9 laisse 0,83 ms. REALTIME y divise quand même la
  moyenne par deux (7,7-9,4 contre 18-21 ms).
- **Les encodeurs** :
  - RTX : D3D12 VE reste plus lent que NVENC, au repos (9,7 contre 2,7 ms)
    et sous RE9 (4,75 contre 1,76). La route D3D12 de la RTX est
    NVENC-D3D12 (phase 7).
  - Arc : D3D12 VE ne dépend pas de la classe (3,7 ms sous RE9 en REALTIME
    comme en HIGH) : il n'utilise que le moteur vidéo. Sa moyenne bat celle
    d'oneVPL-D3D11 au repos (6,0 contre 7,5 ms, §8n.0), son p99 non (29
    contre 24, le bruit connu de l'Arc au repos).
  - iGPU AMD : D3D12 VE ne voit pas la charge (9,0 ms) ; AMF-DX12 gagne au
    repos (5,3) et perd sous charge (21-22 ms, il attend la 3D). G4
    tranchera.
- **La poignée de main** : `ddasync=gpu` ne lit jamais faux — 0 sur 14 879
  lectures, en REALTIME comme en HIGH, au repos, sous charge synthétique et
  sous RE9 — pour 0,2 à 0,3 ms de tenue. Sans elle, en
  REALTIME, la lecture D3D12 passe devant l'écriture de DWM : de 46 à 99 %
  d'images fausses dès le repos. `cpu` bloque le fil de capture (jusqu'à 1 s
  en HIGH sur l'Arc sous RE9).
- **La cadence de capture sous charge** revient avec REALTIME : 41,5 i/s au
  lieu de 22,1 sur la RTX, 64,5 au lieu de 38 sur l'iGPU AMD. Le §8n.1 le
  supposait : la limite était la classe, pas l'interop.

**Verdict G1 (recommandation ; Bruno tranche).** Selon les critères du plan
(§5) :
- **RTX** : conversion D3D12 gardée (PS DIRECT en `GLOBAL_REALTIME`, à
  égalité avec D3D11 sous RE9, 0,83 contre 0,92 ms de p99) ; encodeur
  NVENC-D3D12 (phase 7), D3D12 VE en repli seulement. Pas de compute.
- **Arc** : conversion D3D12 gardée, avec de la marge (p99 3,54 contre 5,75
  ms sous RE9) ; encodeur D3D12 VE, avec le contrôle de débit maison (phase
  6). Pas de compute : C3.4 est abandonné pour Intel.
- **iGPU AMD** : conversion D3D12 à égalité en PS (HIGH, faute de
  `GLOBAL_REALTIME`), meilleure en compute `GLOBAL_REALTIME` sous charge
  synthétique. C3.4 ne sert qu'ici, à confirmer en G2 ; encodeur D3D12 VE
  ou AMF-DX12 (G4).
- **Files** : `CreatorID` propre et `GLOBAL_REALTIME` quand le processus a
  REALTIME, avec repli en HIGH (la politique de `D3d12Device`, décision
  §9-1 du plan).
- **Poignée de main** : `ddasync=gpu`, obligatoire.
- **Route scindée** (conversion D3D11 puis VE D3D12) : inutile, la
  conversion D3D12 ne perd nulle part.
- **Scaler matériel d'Intel (SFC)** : pas justifié sur l'Arc. En REALTIME,
  la conversion ne fait plus la queue derrière le jeu (1,6 ms, p99 3,5 sous
  RE9). À revoir si le N95 montre le contraire.
- **Reste de C1.4** : le N95 (bench-intel), pas encore passé. → Fait le
  27/09 au §8n.8 : là, la conversion attend le jeu même en REALTIME, et le
  SFC revient en question (décision §9-15 du plan).

### 8n.3 La chaîne D3D12 de bout en bout (phase 5, 27/09/2026 →)

**Montage.**
- DualRTX, trois M27Q, un par GPU. `--native-bench` en 2560×1440 à
  120 i/s, 20 Mb/s, chaîne D3D12 forcée (`pipeline=d3d12,strict12=1`).
- Contenu : `scroll.html` ou `still.html` en kiosque sur l'écran capturé.
- Classe GPU HIGH : l'agent a un jeton limité.

**La taille codée (C5.3, `6cf08d3a`).**
- C5.2 laissait sur l'Arc et l'iGPU AMD « quelques images non décodables »
  en 1440p120 : 2 à 6 lignes d'erreur ffmpeg par passe de 8 s. En réalité,
  **toutes les images étaient fausses**.
- Cause : les trois pilotes codent des CTB (blocs de codage) entiers, 32
  pixels sur la RTX, 64 sur l'Arc et l'AMD, quelle que soit la taille
  acceptée. Le produit alignait la taille sur 16 : 1440 lignes font 22,5
  CTB de 64. Le décodeur déduit alors, au bord de l'image, des découpes que
  les tranches du pilote n'ont pas. Tout est faux à partir de la dernière
  rangée de CTB, et la prédiction étend l'erreur à l'écran entier en
  quelques secondes (image 400 : traînées sur l'Arc, blocs verts sur l'AMD).
- **Le compte d'erreurs de ffmpeg ne prouve rien.** Il ne signalait que 2 à
  6 images sur 900. La preuve est au pixel : la sonde `encode` du labo écrit
  ses images d'entrée (`--dump-input`), et `scripts/bench/hevc-psnr.py`
  compare chaque image décodée à la sienne. `--align 16|asked` rejoue
  l'ancienne règle.

PSNR luma médian de la pire bande de 64 lignes, CBR 20 Mb/s :

| cas | taille codée fausse | PSNR | taille codée en CTB entiers | PSNR |
|---|---|---|---|---|
| Arc 2560×1440@120 | 2560×1440 (règle de 16) | 12,2 dB, 448 images sur 448 sous 20 dB | 2560×1472 | 26,4 dB, aucune sous 20 |
| iGPU AMD 2560×1440@120 | 2560×1440 (règle de 16) | 13,5 dB, 360 sur 360 | 2560×1472 | 27,1 dB, aucune |
| RTX 2560×1440@120 | — | — | 2560×1440 | 27,1 dB, aucune |
| Arc 3440×1440@60 | 3440×1440 (règle de 16) | 8,1 dB, image entière à 8,7 | 3456×1472 | 27,0 dB, aucune |
| RTX 3440×1440@60 | 3440×1440 (règle de 16) | 8,1 dB, image entière à 8,7 | 3456×1440 | 27,1 dB, aucune |
| Arc 1920×1080@60 | 1920×1080 (taille demandée) | 12,0 dB | 1920×1088 | 27,1 dB, aucune |
| RTX 1920×1080@60 | 1920×1080 (taille demandée) | 11,9 dB | 1920×1088 | 27,1 dB, aucune |

- Aucune dérive avec des CTB entiers : les 100 premières et les 100
  dernières images ont le même PSNR.
- Le constat du 26/09 (« une image sur 300 » avec un SPS à 1080) était déjà
  ce défaut : toutes les images, à partir de la ligne 1024.
- Correctif : la taille codée est un nombre entier de CTB. Le convertisseur
  remplit la bande en noir, le SPS la recadre. Après correctif, le produit
  sort des flux sans erreur ffmpeg sur les trois GPU, propres jusqu'à la
  dernière ligne.
- Sans lui, un écran ultralarge 3440×1440 aurait donné une image
  entièrement détruite sur les trois GPU, RTX comprise.

**Le QP rapporté (`7d8808a5`).**
- L'Arc et l'iGPU AMD laissent le QP moyen à 0. Le raffinement d'écran fixe
  croyait donc son QP stable et concluait à la 5e passe, quoi que fasse
  l'image.
- L'AMD écrit le QP choisi dans l'en-tête de tranche : il est lu, 18 à 35 au
  test matériel.
- L'Arc écrit `slice_qp_delta = 0` dans toutes ses tranches et porte son QP
  dans les unités de codage, illisibles sans décoder le CABAC. Son QP est
  donc inconnu (-1).

**Le reste de C5.3** (2560×1440@120, 20 Mb/s). Tous les flux se décodent
dans ffmpeg sans erreur.

| essai | RTX | Arc | iGPU AMD |
|---|---|---|---|
| écran fixe (`still.html`) | rafales au plafond de 8 passes, QP 47 → 29 puis 31 → 21 | rafales au plafond, sans budget renforcé (débit non reconfigurable) ; première IDR floue, dernière image nette | 2e rafale convergée en 5 passes (QP 18 → 18) |
| pointeur mobile (`still.html?cursor=1`) | — | 181 réveils « pointeur seul », pointeur dessiné | 183 réveils |
| `lose=45` | 15 pertes sur 15 réparées par une P qui prédit 7 images plus tôt | 14 sur 14 (3 à 7 images) | 15 sur 15 (3 à 5 images) |
| `intra=1` | refusé (aucune image de balayage) → keyframes | refusé (une image) → keyframes | balayage pris |
| `ramp=5000@1` | suivi (7,5 Ko par image en moyenne) | refusé, en attendant la phase 6 | suivi |

- Toutes les rafales de raffinement vont au plafond de 8 passes, sauf une
  sur l'AMD : à comparer à D3D11 en G2.
- Le refus de débit de l'Arc ne s'écrit plus qu'une fois par session : il
  sortait deux fois par pause de la souris.
- Reste : le changement de mode, qui demande un écran virtuel.

### 8n.4 G2 : la chaîne D3D12 contre D3D11, en REALTIME (27/09/2026)

**Montage.**
- Un seul binaire pour les deux bras : `pipeline=d3d11` contre
  `pipeline=d3d12,strict12=1`. Il est figé dans `bench-out\d3d12v2\g2-bin`
  (`987575cf`), et les sorties sont dans `bench-out\d3d12v2\g2`. Le fichier
  `NOTES.txt` y dit quel binaire a fait quel cas.
- Classe GPU REALTIME : un exécuteur élevé, lancé par un clic UAC de Bruno,
  ne fait tourner que `ab-native-bench.ps1`, avec des paramètres vérifiés.
  HAGS actif sur la RTX.
- HEVC à 20 Mb/s, passes de 12 s. Tours alternés : 4 sur la RTX, 8 sur
  l'Arc et au repos.
- Sous RE9, le contenu est le jeu lui-même : sa fenêtre au premier plan
  sur l'écran capturé, rien d'autre qui bouge (`-Content none`). La
  cadence de l'écran est alors celle du jeu, sous les 120 Hz de l'écran.
  Le banc la lit dans le journal du moteur : présentations vues plus
  regroupées, sur la durée de la boucle (colonne `displayFps`, `29fe015a`).
- RTX : réglages de la copie (ray tracing haut). Le jeu tourne à ~63 i/s.
- Arc : avec les mêmes réglages, le jeu y tombe à 6 i/s, et une passe de
  12 s n'a plus que ~75 images, dont le p99 est la pire. Pour ses passes,
  ray tracing coupé et FSR1 en mode performance, le reste au préréglage
  « Highest » : le jeu tourne à ~25 i/s, GPU à 97-100 %. `config.ini` est
  restauré après chaque essai, empreinte vérifiée.
- iGPU AMD : au repos seulement (RE9 n'y tourne pas).

**Deux défauts trouvés par G2**, corrigés avant les chiffres ci-dessous.
- `5e1e0630` : la session prenait sa classe GPU **après** avoir construit
  sa chaîne.
  - Les files D3D12 naissaient donc en HIGH sous REALTIME. Le journal
    disait « DIRECT queue HIGH », et la ligne de classe venait après.
  - Le labo de G1 prenait la classe en premier, d'où l'écart.
  - Les files sont maintenant en `GLOBAL_REALTIME` sur la RTX et l'Arc ;
    l'AMD retombe en HIGH, comme prévu.
  - Sur la RTX, les chiffres ne bougent pas : 5,68 ms en HIGH, 5,70 en
    `GLOBAL_REALTIME`.
- `987575cf` : la chaîne déclarait le GPU perdu après 500 ms sur une
  attente, et la session repassait en D3D11 pour de bon.
  - La première endurance l'a fait au bout de 88 s.
  - Or D3D11, dans les 28 minutes qui ont suivi, a attendu plus de 500 ms
    à 47 reprises, jusqu'à 1,5 s, et a continué : le GPU était occupé, pas
    perdu.
  - La limite passe à 3 s, au-delà du TDR de Windows (2 s), qui retire le
    device d'un GPU vraiment bloqué.

**Sous RE9.** `host_total` moyen / p99 en ms (moyenne des passes), images
capturées par seconde, cadence du jeu.

| cas | tours | D3D11 | D3D12 | écart moyenne / p99 | i/s capturées | jeu (i/s) | critères G2 |
|---|---|---|---|---|---|---|---|
| RTX 1080p60 | 4 | 2,24 / 3,53 | 5,70 / 7,02 | +3,46 / +3,49 | 56,0 → 56,4 | 62,4 → 62,8 | moyenne et p99 non tenus |
| RTX 1080p120 | 4 | 2,24 / 3,53 | 5,69 / 7,01 | +3,45 / +3,48 | 62,0 → 61,5 | 62,4 → 62,4 | moyenne et p99 non tenus |
| Arc 1080p60 | 8 | 10,79 / 48,7 | 5,94 / 29,6 | −45 % / −39 % | 23,6 → 24,3 | 25,2 → 25,1 | tous tenus |
| Arc 1080p120 | 8 | 11,59 / 39,3 | 5,59 / 25,1 | −52 % / −36 % | 23,6 → 24,1 | 24,9 → 24,8 | tous tenus |

- Sur toutes les images de chaque bras, le p99 dit la même chose : RTX
  3,5 → 7,0 ms, Arc 40,4 → 28,7 (60 i/s) et 40,1 → 22,0 (120 i/s).
- Sur la RTX, D3D12 Video Encode coûte 5,3 ms par image, contre 1,9 pour
  NVENC. La conversion D3D12 (0,28 ms) n'y est pour rien.
- Sur l'Arc, la plus longue image D3D12 de ses 16 passes fait 237 ms ;
  côté D3D11, 855 ms.
- Le jeu ne perd rien avec D3D12 : sa cadence reste à 0,7 % près sur les
  deux GPU.

**Au repos** (page qui défile à 120 Hz), 8 tours.

| cas | D3D11 | D3D12 | écart moyenne | i/s capturées | critère « repos » (+0,2 ms) |
|---|---|---|---|---|---|
| RTX 1080p60 | 2,13 / 3,56 | 7,92 / 9,50 | +5,79 | 60,0 → 60,0 | non tenu |
| Arc 1080p60 | 6,30 / 18,6 | 6,25 / 21,3 | −0,05 | 58,6 → 58,9 | tenu |
| iGPU AMD 1080p60 | 8,67 / 14,7 | 15,34 / 21,3 | +6,67 | 59,9 → 59,9 | non tenu |
| iGPU AMD 1080p120 | 8,50 / 14,6 | 16,61 / 22,6 | +8,11 | 116,1 → 79,8 | non tenu |

- Arc : le p99 est bruité (+14 % en moyenne des passes, −4 % sur toutes
  les images). Au tour 7 (bras D3D12) et au tour 8 (les deux bras), l'Arc
  monte à 10-12 ms, quelle que soit la route.
- iGPU AMD : Video Encode prend 12 à 13 ms par image. À 120 i/s, la
  chaîne ne tient plus la cadence.
- Au repos, la cadence de l'écran diffère de 5,8 % sur l'Arc (121,8 contre
  114,7). Ce n'est pas le bureau qui ralentit : les présentations vues sont
  les mêmes (~1350 par passe), seules les regroupées changent (~120 contre
  ~46), et leur nombre suit le rythme de la boucle. D3D11 compte même 123
  i/s sur un écran à 120 Hz. À 25 i/s sous RE9, où presque rien n'est
  regroupé, la mesure est juste.

**Endurance** : Arc sous RE9, 1080p60, 30 min, route D3D12 telle que le
produit la prend (sans `strict12`), flux enregistré.
- 1re (`5e1e0630`) : D3D12 tient 88 s (5,16 ms, p99 10,5, max 55), puis
  une attente passe 500 ms et la session repasse en D3D11 (défaut corrigé
  par `987575cf`, plus haut). D3D11 fait ensuite 10,46 ms, p99 28,6,
  p99,9 542, max 1486.
- 2e (`987575cf`) : pas de repli sur délai. À 160 s, la duplication est
  perdue, puis refusée (`0x80070005`, la réponse de Windows quand le bureau
  affiché n'est pas celui de l'utilisateur : invite UAC, verrouillage,
  Ctrl+Alt+Suppr). La capture passe alors sur Windows.Graphics.Capture,
  qui ne livre qu'à D3D11, et y reste jusqu'à la fin.
  - Le worker du produit installé est SYSTEM et suit le bureau sécurisé
    (`attachThread`). Ce repli ne touche que les workers non SYSTEM : le
    banc, l'édition `--dev`.
  - Défaut à part, pour la phase 8 : après un seul refus, un worker non
    SYSTEM ne revient jamais à la duplication, ni à D3D12.
- 3e (`987575cf`) : **27 min de D3D12 sans repli** (1622 s, 36 143
  images). Moyenne 7,26 ms, p99 12,0, p99,9 59,9, max 185 ; aucune
  attente au-delà de 500 ms. À 27 min, le même refus fait passer la
  capture sur WGC, donc en D3D11, pour les 3 dernières minutes. Les
  overlays de NVIDIA et d'AMD redémarrent dans les 20 s qui suivent : c'est
  un événement de la session, pas du banc.
- Les trois flux se décodent sans erreur dans ffmpeg (39 000 à 43 000
  images chacun). Les images relevées toutes les 5 min sont propres
  jusqu'à la dernière ligne.

**Ce que G2 ne mesure pas encore.**
- Le clic → photon : seule la part de l'hôte (`host_total`) est mesurée.
  Le reste de la chaîne (réseau, décodage, affichage) reçoit le même flux.
  À mesurer en C5.7, sur l'édition installée, dont le worker SYSTEM a
  REALTIME.
- La qualité sur l'Arc : son QP est inconnu (-1), et ses images D3D12 sont
  à la taille plafond (32,5 Ko en 1080p60, contre 40,0 pour oneVPL ; 20,3
  contre 12,9 en 1080p120). À juger avec le contrôle de débit maison
  (phase 6, G3).
- La colonne `pipeline` du banc donne la route de départ, pas la route
  courante : un repli ne se voit que dans le journal.

**Verdict G2 (recommandation ; Bruno tranche).**
- **RTX** : D3D12 Video Encode n'est pas candidat au défaut. Il coûte
  +3,5 ms sous RE9, +5,8 ms au repos, et double le p99. Le jeu n'y perd
  aucune image. La RTX reste en D3D11 (NVENC) ; sa route D3D12 est
  NVENC-D3D12 (phase 7, G4).
- **Arc** : candidat. Sous RE9, −45 à −52 % sur la moyenne et −36 à −39 %
  sur le p99, pour un jeu qui ne perd rien ; au repos, égal en moyenne.
  En endurance, 27 min sans repli ni image fausse. Les 30 min du critère
  n'ont pas été atteintes : deux fois, la duplication a été refusée hors
  du bureau de l'utilisateur, ce qu'un worker SYSTEM aurait traversé.
- Le passage de l'Arc à D3D12 par défaut attend de toute façon le contrôle
  de débit maison (phase 6, G3) : D3D12 Video Encode ne change pas de
  débit sur l'Arc.
- **iGPU AMD** : pas candidat. +6,7 ms à 60 i/s, +8,1 ms et 31 % d'images
  en moins à 120 i/s. Reste AMF-DX12 (G4).

### 8n.5 C5.3 bis et C5.4 : changement de mode et HDR, sur l'écran virtuel du produit (27/09/2026)

**Montage.**
- L'édition dev de la branche est installée sur le poste de banc
  (`0.3.1-4948717d-dev`, installeur construit en local comme en CI). Son
  worker est celui du produit, avec la classe REALTIME. Le réglage C5.6
  (`native_video_pipeline=d3d12`) passe par l'API locale, clé d'admin
  comprise. Il a été remis sur `auto` après les passes.
- L'écran virtuel du produit est rendu par l'Arc : c'est la chaîne D3D12 de
  l'Arc qui est testée. La RTX et l'iGPU AMD restent en D3D11 (G2).
- Le client kiosque décode sur l'iGPU AMD. Aucun écran physique n'a changé
  de mode ni de HDR.

**C5.3 bis — changement de mode (`display-follow.ps1`, `bench-out\d3d12v2\c53bis`).**
- 7 changements, 0 KO : lancement, passage en 16:9 en plein stream, retour
  en 4:3, HDR de l'hôte allumé puis éteint (flux SDR, tone-mappé sur
  l'hôte), lancement sur un écran déjà en HDR.
- Les délais sont ceux de D3D11 dans les mêmes conditions (`results-c51`,
  25/09) : 17,7 s pour un changement de forme, 8,7 s pour un lancement.
  C'est la cadence de sondage de l'instrument.
- Le journal du worker dit « video pipeline: D3D12 (DIRECT conversion →
  D3D12 Video Encode HEVC), because the setting (d3d12) » et « streaming …
  · D3D12 VE HEVC » : le réglage du produit arrive jusqu'au moteur.
- Huit reconstructions de l'encodeur, toutes en D3D12, toutes en CTB
  entiers (1472×1088, puis 1472×832 en 16:9). Aucun repli.
- Deux reconstructions par changement : la nouvelle forme, puis le
  redémarrage de la duplication. C'est C11.4.

**C5.4 — vrai flux HDR (`--native-bench hdr=1`, `bench-out\d3d12v2\c54`).**
- Un stream de l'édition dev tient l'écran virtuel allumé, passé en HDR.
  Deux passes de 8 s du banc sur la même image fixe (`still.html`) : D3D11
  (oneVPL) puis D3D12 (`strict12=1`).
- Les deux flux ont la même VUI : HEVC Main 10, 4:2:0 10 bits, primaires
  BT.2020, transfert PQ (SMPTE 2084), matrice BT.2020 NCL, plage TV. Aucun
  n'a de SEI HDR10. Zéro erreur de décodage dans ffmpeg.
- Les niveaux sont les mêmes. Moyennes Y 554,3 contre 554,8 (le blanc du
  bureau, 240 nits), U et V neutres. Les moyennes par blocs s'accordent à
  57 dB (8×8) et 64 dB (32×32).
- Sur le détail, l'écart est de 39 dB. La cause est le débit du pilote de
  l'Arc, pas la chaîne HDR : chaque renvoi de l'écran fixe fait ~33 Ko,
  l'image ne s'affine pas, et un trait d'un pixel reste effacé. En D3D11,
  les renvois tombent à 0-1 Ko une fois l'image convergée. C'est le constat
  de C5.3 en SDR ; la phase 6 (G3) le corrige.
- Piège : sondé 4 s après la bascule en HDR, l'écran virtuel passe encore
  pour SDR, et les deux bras sortent en SDR. 8 s suffisent.

**Reste.** Le HDR de la chaîne D3D12 sur la RTX et l'iGPU AMD, qui ne sont
pas candidats (G2). Le test manuel de Bruno (C5.7), sur cette même
installation.

### 8n.6 G3, première partie : le contrôle de débit maison sur l'Arc, au banc (27/09/2026)

**Montage.**
- `--native-bench` sur l'écran de l'Arc, 1920×1080 tiré d'un bureau
  2560×1440, 20 Mb/s, classe GPU HIGH (jeton de l'agent). Passes de 12 à
  24 s, sorties dans `bench-out\d3d12v2\g3`, lues par
  `scripts/bench/rate-report.py`.
- Contenus : `scroll.html` (texte qui défile), le clip Call of Duty,
  `scroll.html?pause=2` (2 s de défilement, 2 s d'arrêt), `still.html`,
  `ramp=5000@2` (marches 20 ↔ 5 Mb/s), `lose=45`.
- `governor=0` (`9099d229`) : sans récepteur, le gouverneur du lien coupait
  le débit à 80 % au bout de 4 s de chaque passe, et ne transmettait jamais
  une marche de `ramp=` vers le haut. Toutes les passes d'avant ce commit
  avaient cette coupure ; en G2, le bras D3D11 l'a subie, pas le bras D3D12
  de l'Arc, qui refusait tout changement de débit.
- Critères G3 (plan §5) : débit à ±10 % sur des fenêtres de 2 s ; p95 de la
  taille d'image ≤ 2 × budget ; marche de débit suivie en 3 images ; écran
  fixe convergé à QP 18 ; pas de pompage visible (test de Bruno) ; latence
  pas pire qu'en G2.

**Référence D3D11 (oneVPL) et D3D12 avec le débit du pilote, mêmes passes.**
- D3D11 : défilement 60 i/s, 2 fenêtres sur 9 à ±10 % (0,69 à 0,98 de la
  cible), p95 1,53 × le budget ; clip, 3 sur 9, p95 1,39 ; rampe, marches
  suivies 8 fois sur 10 ; écran fixe, rafales au plafond de 8 passes.
- D3D12 avec le débit du pilote : des images constantes de 33 Ko (0,80 du
  budget), quelle que soit la cible : le pilote de l'Arc ne change pas de
  débit en cours de séquence.
- `host_total` : 10 à 15 ms en moyenne, p99 33 à 43 ms, pour les deux. Dans
  chaque passe qui utilise le contrôle de débit du pilote (oneVPL ou D3D12
  CBR), l'encodage de l'Arc passe de ~5 à ~18 ms entre 6 et 8,5 s. Jamais
  en CQP. G2, en REALTIME, n'avait pas vu cela (6,3 ms au repos) : à vérifier
  en REALTIME.

**Ce que les premiers essais ont appris** (`89bcf517`, `6f9c6c13`, corrigés
par `8d2e6bfc`).
- Un plancher « un quart de l'intra » tenait le texte qui défile à QP 38 à 45,
  pour un dixième du débit : une page qui défile ne coûte qu'un trentième de
  son image intra (compensation de mouvement).
- Sans plancher, le texte oscillait image par image entre 4 à 13 budgets et
  presque rien. La taille ne suit pas « moitié tous les 6 QP » : 162 Ko à
  QP 24, presque rien à QP 36. La pente se lit maintenant sur les images
  elles-mêmes (sécantes, 2 à 2,5 QP par moitié pour le texte).
- À 120 i/s, une capture sur deux regroupe deux présentations et coûte 2,5
  fois la suivante. Remplacer le modèle à chaque écart de plus de 2×
  doublait l'oscillation : il faut deux écarts de suite dans le même sens.
- Un pic suivi d'une image quasi vide 17 QP plus haut n'est pas un « pic
  ponctuel » : pris pour tel, il ramenait l'ancienne croyance image après
  image, et le QP descendait à 18 (rejoué depuis le CSV réel).
- Écran fixe : une passe coûte la part de l'image qu'elle affine,
  (2^(ΔQP/6) − 1) de l'image entière. Prise pour une image entière, la page
  de texte s'arrêtait au QP de sa première image, 42, « convergée ».

**Le contrôle maison, version du `8d2e6bfc`.**

| contenu | fenêtres à ±10 % | taille / budget : moy. / p95 | très au-dessus (> 2,5×) | QP moyen | `host_total` moy. / p99 (ms) |
|---|---|---|---|---|---|
| défilement 60 i/s | 7 / 9 (0,86 à 0,95) | 0,92 / 1,86 | 12 | 30 | 5,8 / 11,4 |
| défilement 60 i/s, `reencode=1` | 5 / 9 (0,78 à 0,93) | 0,88 / 1,85 | 1 | 30 | 5,8 / 10,1 |
| clip de jeu 60 i/s | 8 / 9 (0,72 à 1,03) | 1,04 / 1,72 | 5 | 23 | 5,6 / 7,7 |
| clip de jeu, `reencode=1` | 4 / 9 (0,78 à 0,97) | 0,97 / 1,65 | 0 | 23 | 5,6 / 7,6 |
| défilement 120 i/s | 7 / 9 (0,85 à 0,93) | 0,97 / 2,33 | 82 | 35 | 5,3 / 11,9 |
| `lose=45` | 4 / 5 (0,84 à 0,96) | 0,92 / 1,86 | 7 | 30 | 5,5 / 7,7 |

- Rampe 20 ↔ 5 Mb/s : marches suivies en 3 images 9 fois sur 10 (D3D11 :
  8 sur 10) ; p95 2,61, les images de transition comprises.
- Écran fixe : QP 42 → 18 en 8 passes (116 Ko puis 642 Ko), « converged ».
- Pause puis défilement : la première image après l'arrêt fait jusqu'à 8
  budgets sans ré-encodage, 2,7 au plus avec.
- Le pilote code le QP demandé sur chaque image : jamais d'écart relevé, Main
  10 compris (test matériel).
- Les pertes sont réparées par invalidation, comme en G2.
- Les fenêtres hors des ±10 % le sont toutes par défaut de débit (0,72 à
  0,86), jamais par excès.

**Critères G3, sur l'Arc au banc.**
- Débit à ±10 % : atteint la plupart du temps, mieux que D3D11 dans les mêmes
  passes ; les ratés sont sous la cible.
- p95 ≤ 2 × budget : tenu à 60 i/s (1,65 à 1,86) ; pas à 120 i/s sur du texte
  (2,33), ni sur la rampe (2,61).
- Marches suivies en 3 images : tenu (9 sur 10).
- Écran fixe à QP 18 : tenu.
- Latence : 5,3 à 5,8 ms de moyenne, contre 6,25 pour D3D12 en G2 et 10 à
  15 ms pour D3D11 dans ces passes.
- Pompage : le test de Bruno le dira.

**`reencode=1` (décision §9-5 du plan).** Il retire presque tous les
dépassements forts (défilement : 12 → 1, clip : 5 → 0, première image après
une pause : 8 → 2,7 budgets), pour un encodage de plus sur ces images
(`host_total` moyen inchangé). Recommandation : l'activer par défaut.
**Décision de Bruno (27/09) : actif par défaut** ; `reencode=0` le retire
au banc. En l'activant, un défaut est apparu sur les passes d'écran fixe.
Une passe très au-dessus de son budget était recodée par la règle des
nouvelles images, donc au-dessus du QP de l'image : elle ne codait rien et
n'apprenait rien, et la passe suivante retentait le même pas. Sur le
simulateur, une page de texte dense restait au QP du défilement (31) au lieu
de 21, et chaque passe était codée deux fois. Corrigé dans le même commit :
la passe qui dépasse apprend ce que coûte l'image entière, puis elle est
replanifiée sans jamais dépasser le QP de l'image. Les passes `pause` du banc
n'avaient pas rencontré ce cas : leurs passes faisaient 100 à 160 Ko, pour un
budget de rafale de 125 Ko.

**Reste pour G3.** Les passes en REALTIME et sous RE9 (exécuteur élevé, un
clic UAC de Bruno), qui diront aussi si la lenteur du contrôle de débit du
pilote Intel tient en REALTIME ; le N95 ; la RTX en témoin ; le profil
« Internet » sur un vrai stream (pertes, RTT, marches de bande passante,
gouverneur actif) ; puis le test manuel de Bruno sur une édition dev
installée.

### 8n.7 G3, deuxième partie : en REALTIME et sous RE9 (27/09/2026)

**Montage.**
- Exécuteur élevé (un clic UAC de Bruno), classe GPU REALTIME. Binaires
  figés dans `bench-out\d3d12v2\g3-bin` (`b3d9aefb`, ré-encodage actif par
  défaut). Écran de l'Arc, 1920×1080, 20 Mb/s, `governor=0`. Sorties dans
  `bench-out\d3d12v2\g3rt`.
- Bureau : les passes de la première partie, plus leurs références D3D11
  (oneVPL) et D3D12 avec le débit du pilote.
- RE9 (la copie) sur l'Arc, en réglages légers (~25 i/s) : A/B D3D11 contre
  D3D12 en 4 tours à 60 et 120 i/s, passes de débit, rampe et pertes, puis
  une endurance de 15 min avec le flux enregistré.

**Bureau, en REALTIME.**

| contenu | fenêtres à ±10 % | taille / budget : moy. / p95 | très au-dessus (> 2,5×) | `host_total` moy. / p99 (ms) |
|---|---|---|---|---|
| défilement 60 i/s | 5 / 9 (0,78 à 0,96) | 0,90 / 1,74 | 2 | 4,0 / 6,5 |
| défilement 60 i/s, `reencode=0` | 7 / 9 (0,84 à 0,96) | 0,93 / 1,97 | 21 | 3,9 / 4,7 |
| défilement 120 i/s | 4 / 9 (0,82 à 0,94) | 0,90 / 1,86 | 1 | 4,1 / 6,4 |
| clip de jeu 60 i/s | 9 / 9 (0,90 à 0,95) | 0,94 / 1,33 | 0 | 4,0 / 4,8 |
| `lose=45` | 2 / 5 (0,80 à 0,95) | 0,88 / 1,84 | 0 | 4,1 / 10,6 |
| D3D11, défilement 60 i/s | 2 / 9 (0,71 à 0,98) | 0,94 / 1,50 | 0 | 17,9 / 42,3 |
| D3D11, défilement 120 i/s | 2 / 3 (0,56 à 0,96) | 0,90 / 1,47 | 0 | 15,4 / 43,2 |
| D3D11, clip de jeu | 3 / 9 (0,76 à 0,98) | 0,99 / 1,32 | 0 | 10,7 / 30,1 |
| D3D12 au débit du pilote, défilement 60 i/s | 0 / 9 (0,71 à 0,80) | 0,80 / 0,80 | 0 | 11,3 / 31,7 |

- Rampe 20 ↔ 5 Mb/s : marches suivies en 3 images 8 fois sur 9, 4 images
  très au-dessus. D3D11 : 7 sur 10, et 49 images très au-dessus.
- Pause puis défilement : la première image après l'arrêt fait au plus 1,1
  budget (jusqu'à 8 sans ré-encodage, en première partie).
- Écran fixe : QP 45 → 18, puis des renvois de 2,3 Ko à QP 18.
- La lenteur du contrôle de débit Intel existe aussi en REALTIME. En D3D11
  (oneVPL), l'encodage passe de 5-7 ms à 19 ms à partir de 6 s de
  défilement, à 11-12 ms sur le clip. En D3D12 au débit du pilote, il passe
  de 4-6 ms à 13-15 ms à partir de 8 s. Notre QP constant reste à 3,7 ms du
  début à la fin. C'est l'essentiel de l'écart de latence, et le chemin
  D3D11 que l'Arc prend aujourd'hui par défaut en souffre sur tout contenu
  animé.

**Le débit reste sous la cible, et c'est structurel.** Toutes les fenêtres
hors des ±10 % sont sous la cible. Le contrôleur tient pourtant son budget
(taille / budget de 1,01 à 1,04). Mais ce budget ne vaut en moyenne que
0,86 à 0,91 fois la cible, pour trois raisons :
- le seau n'est presque jamais vide (0,2 à 0,3 image en moyenne) ;
- le budget en retranche la moitié ;
- une image sous son budget quand le seau est vide ne se rattrape jamais.

Un rejeu en boucle fermée des images réelles
(`bench-out\d3d12v2\g3rt\replay\loop.cpp`, hors dépôt) reproduit la
moyenne : 0,895, contre 0,897 mesuré. Il chiffre aussi une variante : un « crédit » d'un quart d'image
sous le seau vide.
- Gain : la moyenne passe à 0,94-0,97, avec 7 à 9 fenêtres sur 9 dans les
  ±10 %.
- Coût : des images plus grosses (p95 +0,1 ; à 120 i/s, 15 images au-delà
  de 2,5 budgets au lieu d'une), donc de la latence sur ces images.

Recommandation : garder le budget actuel, latence d'abord (décision §9-13
du plan).

**Sous RE9, en REALTIME.**

| passe | D3D12, notre débit : moy. / p99 | D3D11 : moy. / p99 | G2, D3D12 au débit du pilote |
|---|---|---|---|
| A/B 1080p60, 4 tours | 5,7 / 16,5 ms | 12,8 / 62,0 ms | 5,9 / 29,5 ms |
| A/B 1080p120, 4 tours | 4,5 / 14,1 ms | 9,1 / 33,4 ms | 5,6 / 25,0 ms |

- À 60 i/s, tous les critères de G2 sont tenus. À 120 i/s aussi, sauf la
  cadence du jeu : −2,5 % (25,8 contre 26,5 i/s). C'est du bruit de tour à
  tour : l'A/B à 60 i/s donnait l'écart inverse (+15 %).
- Le jeu ne livre que ~25 images par seconde, et la session double le
  budget de l'encodeur (`EffectiveCadence` : « frames arrive at 30 fps for
  a 60 fps stream »).
  - À 20 Mb/s, le contenu tient au plancher : QP 18, 31 Ko par image, ~6
    Mb/s sur le fil.
  - Rampe 20 ↔ 5 Mb/s : chaque marche est suivie en 1 à 2 images, contre le
    budget de l'encodeur (~20 Ko à QP 20-22).
  - `rate-report.py` compte contre le débit du fil : il se trompe sur ces
    passes.
- Endurance de 15 min : 22 540 images, 4,2 / 8,6 ms. Aucun repli, aucun
  écart de QP du pilote, aucun dépassement fort. ffmpeg décode les 742 Mo
  sans erreur.

**Critères G3 sur l'Arc.**
- Débit à ±10 % : tenu sur le clip de jeu (9 sur 9). Sur du texte, 4 à 5
  fenêtres sur 9, toutes sous la cible (0,78 à 0,89) : c'est structurel
  (voir plus haut). D3D11 fait moins bien (2 à 3 sur 9).
- p95 ≤ 2 × budget : tenu en REALTIME à 60 et 120 i/s (1,33 à 1,86). La
  rampe monte à 2,07, à cause des images de transition.
- Marches suivies en 3 images : tenu (8 sur 9 ; sous RE9, 1 à 2 images).
- Écran fixe à QP 18 : tenu.
- Latence pas pire qu'en G2 : tenu, et de loin. Au p99 sous RE9 : 16,5
  contre 29,5 ms à 60 i/s, 14,1 contre 25,0 ms à 120 i/s.
- Pompage : c'est le test de Bruno qui le dira.

**Reste pour G3.**
- Le N95 (mw-intel) : fait au §8n.8.
- La RTX en témoin : son écran est l'écran principal de Bruno.
- Le profil « Internet » sur un vrai stream, gouverneur actif.
- Le test de Bruno sur l'édition dev.

### 8n.8 Le N95 (mw-intel) : G2 et G3 sur un iGPU Intel (27/09/2026)

**Montage.**
- mw-intel : Intel N95 (Alder Lake-N), UHD Graphics `0x46D2`, pilote
  32.0.101.7088, sans HAGS (le pilote ne le propose pas). Écran capturé : la
  sortie de l'UHD, 1920×1080 à 60 Hz.
- Binaires figés de G3 (`b3d9aefb`), copiés dans `C:\Users\max\mw-d3d12`.
  L'exécuteur tourne élevé, par une tâche planifiée dans la session console
  de max (SSH arrive en session 0, sans écran) : classe GPU REALTIME, comme
  le worker installé. Même liste fermée de clés que sur DualRTX.
- HEVC 1080p60, 20 Mb/s, `governor=0`. Contenus dans Chrome en kiosque sur
  l'écran capturé. Charge synthétique : `mw-gpu-load` au niveau 1,05, en
  plein écran ; cet iGPU n'a pas de capteur de température, donc 60 s au
  plus par lancement. Pas de vrai jeu (décision §9-4 du plan en attente).
- Sorties dans `bench-out\d3d12v2\n95` : `g3` pour les passes et les A/B,
  `g1` pour les sondes du labo, `replay` pour le rejeu.

**Ce que l'UHD offre** (sonde `caps`, tests natifs).
- D3D12 Video Encode en HEVC Main, Main 10 et H.264 ; pas d'AV1 en
  encodage.
- Pas de reconfiguration du débit (drapeaux 0x774d, comme l'Arc) : la chaîne
  D3D12 y prend notre contrôle de débit, en CQP.
- CTB de 64 : 1080 est codé en 1088, 1440 en 1472.
- La surface DDA s'ouvre dans D3D12. Poignée de main au repos : 99 µs en
  moyenne, 885 µs au p99 (7 et 18 µs sur l'Arc).
- `GLOBAL_REALTIME` est refusé en jeton limité et accordé en REALTIME.
- Tests natifs des groupes D3D12 sur le N95 : 622 vérifications, 0 échec
  (négociation, device, interop DDA, conversion, encodeur sur le vrai
  pilote).
- ⚠️ La sonde `caps` du labo plantait dans le pilote (`igd12dxva64.dll`,
  violation d'accès).
  - Le labo sans tampon de sortie (`7a9974c2`) a trouvé la requête : le
    support du mode `absolute-qp-map` en HEVC. La même requête avec le
    drapeau EXTENSION1 obtient une réponse. Le produit ne fait ni l'une ni
    l'autre.
  - Depuis `a0fd976f`, le labo rapporte la faute comme réponse
    (0xC0000005) et continue : Main 10 comme Main, 4:4:4 sans configuration
    acceptée, H.264 High.
- Le N95 énumère l'UHD quatre fois : quatre LUID distincts, un seul avec
  les écrans. Trois pilotes d'écran virtuel IddCx y sont actifs (Parsec,
  Virtual Display Driver, SudoMaker).
  - Les tests natifs « sur chaque GPU » y tournent donc quatre fois.
  - La sonde du produit liste quatre UHD.
  - Ce sont les adaptateurs des pilotes d'écran virtuel eux-mêmes. Le
    noyau les dit « affichage indirect » sans rendu, avec l'adresse PCI de
    l'UHD comme adaptateur de rendu. DXGI leur donne le nom de l'UHD et
    range leurs écrans sous l'UHD. DualRTX en a un aussi, Parsec, listé
    comme une seconde Arc A380.
  - Chaque sonde du produit (rafraîchissement de la liste des hôtes,
    démarrage de session) ouvrait une session oneVPL sur chacun.
  - ✅ Corrigé par `a83087db` : la sonde écarte un adaptateur d'écran
    virtuel qui ne porte aucun écran, et le dit une fois par sonde. Sur le
    N95, 1 GPU au lieu de 4, la sonde en 0,8 à 1,0 s au lieu de 2,8 s,
    tests natifs 5500/5500.
- Flux : 10 s de défilement enregistrées, décodées par ffmpeg sans erreur ;
  image nette jusqu'en bas (1088 recadré en 1080).

**Au repos : A/B D3D11 (oneVPL) contre D3D12, défilement, 4 tours
alternés.**

| | D3D11 | D3D12 |
|---|---|---|
| `host_total` moy. / p99 | 13,2 / 40,6 ms | 9,5 / 38,6 ms |
| encodage moy. / p99 | 11,2 / 28,4 ms | 7,5 / 14,2 ms |
| images capturées par seconde | 56,3 | 56,3 |
| Ko par image | 39,6 | 30,2 |

- Les critères de G2 sont tenus, sauf la « cadence du jeu ». Avec Chrome
  comme contenu, ce compteur n'est pas une cadence d'affichage :
  - D3D11 y compte 62,8 à 63,8 présentations par seconde, sur un écran à
    60 Hz.
  - Sa boucle, plus lente, replie plus de présentations par capture (65 à
    103, contre 29 à 43), et le compte des présentations repliées déborde.
  - Les deux chaînes capturent le même nombre d'images.
- L'A/B sur écran fixe ne mesure rien : une seule capture par passe.

**Sous charge 3D synthétique : 4 tours alternés de 15 s.**

| | D3D11 | D3D12 |
|---|---|---|
| `host_total` moy. / p99 | 32,8 / 64,4 ms | 31,3 / 62,7 ms |
| encodage moy. | 30,3 ms | 29,4 ms |
| images capturées par seconde | 28,4 | 28,6 |
| i/s de la charge | 30,5 | 30,9 |

L'iGPU est saturé. Le temps d'encodage, qui compte aussi l'attente de la
conversion dans les deux chaînes, passe de 7 à 29-30 ms. D3D12 garde un
léger avantage (−1,4 ms en moyenne), loin des −45 à −52 % de l'Arc sous RE9.
Les sondes ci-dessous disent où part le temps : dans la conversion, sur le
moteur 3D, pas dans l'encodeur.

**Sondes du labo (C1.4), jeton limité puis élevé** (`bench-out\d3d12v2\n95\g1`).
Conversion 2560×1440 → 1920×1080 Lanczos-2 avec pointeur, 120 soumissions
par seconde ; charge `mw-gpu-load` au niveau 1,05. Temps en ms, moyenne /
p99.

| sonde | repos, HIGH | repos, REALTIME | charge, HIGH | charge, REALTIME |
|---|---|---|---|---|
| conversion D3D11 | 11,3 / 12,8 | 12,3 / 15,3 | 169 / 337 | 18,9 / 38,5 |
| conversion D3D12, PS sur DIRECT | 12,8 / 13,9 | 13,3 / 14,7 | 158 / 317 | 19,1 / 37,4 |
| attente de la file D3D12 | 0,2 | 0,2 | 49 | 5,2 (10,3 en HIGH) |
| VE seul | 5,7 / 8,1 | 5,7 / 8,5 | 5,5 / 13,6 | 4,6 / 11,1 |
| VE après la conversion | 15,5 | 15,5 | 197 | 30,5 |

- Le N95 ne tient pas 120 conversions par seconde de ce format (13 ms de GPU
  chacune) : la sonde sature l'iGPU à elle seule. La charge, 47 i/s seule,
  tombe à ~40 i/s face à la sonde en HIGH, et à 11-19 i/s en REALTIME, où la
  conversion passe devant. Seules les comparaisons entre lignes comptent.
- Comme sur DualRTX (§8n.2), c'est la classe qui décide. Sous charge, la
  conversion passe de 160-345 ms en HIGH à ~19 ms en REALTIME. La file
  `GLOBAL_REALTIME` attend deux fois moins que la file HIGH (5,2 contre
  10,3 ms).
- En REALTIME, la conversion D3D12 égale celle de D3D11 sous charge (19,1
  contre 18,9 ms). Au repos, elle coûte 1 ms de plus sur ce format lourd.
  COMPUTE n'apporte rien, comme sur l'Arc.
- L'encodeur VE tourne sur le moteur vidéo : la charge 3D ne le ralentit
  pas (4,6 à 5,7 ms). Ce qui attend le jeu, c'est la conversion, sur le
  moteur 3D. D'où les 29 ms de l'A/B sous charge, et le « ×4 sous charge »
  vu sur ce banc le 22/09.
- La poignée de main DDA est propre au repos (`ddasync=gpu`, 0 lecture
  fausse, 58 i/s). Sous charge, la fenêtre de la charge couvre la page de
  bandes : rien de mesuré.
- Bruno avait posé une condition pour mesurer le scaler matériel d'Intel
  (SFC, sur le moteur vidéo) : « à revoir si le N95 montre une conversion
  qui attend le jeu ». Elle est remplie → décision §9-15 du plan.

**G3 : passes seules, en REALTIME.**

| contenu | fenêtres à ±10 % | taille / budget : moy. / p95 | très au-dessus (> 2,5×) | `host_total` moy. / p99 (ms) |
|---|---|---|---|---|
| défilement | 0 / 9 (0,60 à 0,75) | 0,74 / 2,01 | 2 | 10,1 / 42,0 |
| défilement, `reencode=0` | 4 / 9 (0,76 à 0,91) | 0,91 / 2,53 | 57 | 8,6 / 25,1 |
| clip de jeu | 2 / 9 (0,85 à 0,92) | 0,93 / 1,29 | 0 | 8,4 / 34,9 |
| `lose=45` | 0 / 5 (0,59 à 0,71) | 0,70 / 2,08 | 1 | 9,8 / 29,8 |
| pause puis défilement | 0 / 5 (0,49 à 0,66) | 0,75 / 2,05 | 1 | 10,1 / 27,3 |
| D3D11, défilement | 6 / 9 (0,76 à 0,97) | 0,97 / 1,43 | 0 | 13,8 / 46,5 |
| D3D11, clip de jeu | 9 / 9 (0,93 à 0,98) | 1,00 / 1,23 | 0 | 11,7 / 31,0 |
| D3D12 au débit du pilote, défilement | 0 / 9 (0,73 à 0,78) | 0,80 / 0,80 | 0 | 10,5 / 29,1 |

- Rampe 20 ↔ 5 Mb/s : marches suivies en 3 images 7 fois sur 10, 4 images
  très au-dessus. D3D11 : 5 sur 10, et 50 images très au-dessus.
- Écran fixe : QP 45 → 18, puis des renvois de 9,8 Ko à QP 18.
- Pause puis défilement : la première image après l'arrêt fait de 0,6 à
  1,7 budget.
- Le pilote a codé le QP demandé sur toutes les images. Aucun repli : les
  21 passes D3D12 sont restées en D3D12.
- Le N95 ne capture que 55 à 57 images sur 60. Même avec chaque image à son
  budget, le fil ne porterait que 0,92 à 0,95 de la cible.

**Pourquoi le texte reste loin sous la cible.**
- Sur le N95, Chrome fait défiler la page par à-coups : il partage l'iGPU
  avec la capture et l'encodage. À QP presque égal, les images vont de 1 à
  160 Ko.
  - 32 % des images font 4 fois plus, ou 4 fois moins, que la précédente.
  - Sur l'Arc, c'est 19 %.
  - En D3D11, c'est 2 % : le contrôle de débit du pilote connaît l'image
    avant de la coder.
- Les rafales dépassent 2,5 budgets. Sans ré-encodage, c'est le cas de 97
  images sur 1 144 ; avec, 153 images sur 1 104 sont codées deux fois.
- Le ré-encodage monte le QP à la pente du manuel, 6 QP par moitié. Or le
  texte suit 2 à 2,5 QP par moitié : l'image recodée tombe vers un cinquième
  de son budget, et ces bits ne se rattrapent jamais.
- La même cause joue sur l'Arc, en plus petit. C'est une part du « déficit
  structurel » du §8n.7. Le recul était déjà visible au §8n.6 avec
  `reencode=1` : défilement 0,92 → 0,88, clip 1,04 → 0,97.

**Rejeu en boucle fermée** (`bench-out\d3d12v2\n95\replay`, hors dépôt).
- Même méthode qu'au §8n.7. Le rejeu reproduit les passes, image par image :
  0,745 contre 0,74 mesuré avec le ré-encodage, 0,906 contre 0,91 sans.
- Variante chiffrée :
  - premier ré-encodage à la pente apprise, jamais sous 3, en visant 2
    budgets (sous le seuil de 2,5) ;
  - second essai à la règle d'aujourd'hui, seulement si l'image dépasse
    encore 2,5 budgets.

| passe rejouée | aujourd'hui | variante |
|---|---|---|
| N95, défilement | 0,745 ; 0 / 9 | 0,893 ; 7 / 9 |
| Arc, défilement 60 i/s | 0,902 ; 4 / 9 | 0,922 ; 7 / 9 |
| Arc, défilement 120 i/s | 0,900 ; 6 / 9 | 0,923 ; 7 / 9 |
| clip de jeu, Arc et N95 | 0,934 et 0,922 | 0,936 et 0,929 |

(Taille moyenne sur budget, et fenêtres de 2 s, comptées par image.)
- Aucune image de plus au-delà de 2,5 budgets ; le p95 ne bouge pas (1,3 à
  1,9). Un peu moins d'images recodées (N95 : 153 → 135).
- Le second essai sert aux vraies nouvelles images, comme une page qui
  change : leur taille suit la pente du manuel. On a injecté dans le rejeu
  une nouvelle page par seconde, 8 fois l'image médiane.
  - Sans second essai, elles montent jusqu'à 8,9 budgets sur le N95 et 10,3
    sur l'Arc.
  - Avec lui, elles gardent les mêmes bornes qu'aujourd'hui : 1,0 et 1,4
    budget.
- Coût :
  - une image recodée part à 2 budgets au lieu de 0,2 à 1, donc un peu plus
    de temps d'envoi sur ces images ;
  - un troisième encodage sur les rares images qui dépassent encore.
- → Décision §9-14 du plan.

**Le débit du pilote Intel, en D3D12, donne des images de taille fixe.**
- En CBR avec un VBV d'une image, le pilote du N95 remplit chaque image
  prédite jusqu'à la même taille, quel que soit le contenu :
  - des tranches de 33 269 octets, bourrées de `cabac_zero_words`, sans NAL
    de remplissage ;
  - soit 0,80 du budget.
- C'est le « 0,80 / 0,80 » de l'Arc aux §8n.6 et §8n.7 : même pilote. Le
  flux est valide (ffmpeg, image nette), mais il paie le débit sans rien
  coder de plus.
- Sous ce CBR, le pilote rapporte un `AverageQP` de 184, hors de la plage
  HEVC. Depuis `e66a4b56`, la télémétrie l'ignore au-delà de 51 et lit la
  tranche. Ce chemin n'est pas celui du produit sur Intel.

**Critères G3 sur le N95.**
- Débit à ±10 % : non tenu.
  - Sur du texte : 0 fenêtre sur 9, toutes sous la cible.
  - Sur le clip : 2 sur 9 (0,85 à 0,92).
  - Deux causes : le ré-encodage (corrigeable, §9-14 ; 0,68 → 0,83 avec
    `refit=1`, §8n.9), et les 5 à 8 % d'images que le N95 ne capture pas.
- p95 ≤ 2 × budget : tenu sur le clip (1,29), limite sur le texte (2,01 à
  2,08).
- Marches suivies en 3 images : 7 fois sur 10 (D3D11 : 5 sur 10).
- Écran fixe à QP 18 : tenu.
- Latence :
  - meilleure que D3D11 au repos : 9,5 contre 13,2 ms en moyenne, avec un
    encodage de 7,5 ms contre 11,2 ;
  - à peine meilleure sous une charge 3D qui sature l'iGPU : 31,3 contre
    32,8 ms.
- Aucun repli, flux valide.

**Reste.**
- Le correctif du ré-encodage (§9-14) : fait, passé au banc du N95 (§8n.9).
  Restent ses passes sur l'Arc.
- Le SFC sur le N95, si Bruno le retient (§9-15).
- Le témoin RTX.
- Le profil « Internet ».
- Le test de Bruno.

### 8n.9 Le ré-encodage « sous la ligne » (`refit=1`) au banc du N95 (27/09/2026)

**Montage.**
- Binaires de `e66a4b56`, qui ajoute la clé de banc `refit=0|1` (désactivée
  par défaut). Même exécuteur élevé qu'au §8n.8 : classe REALTIME, HEVC
  1080p60, 20 Mb/s, `governor=0`.
- Passes seules, alternées A-B puis B-A :
  - défilement : 4 passes de 30 s par bras ;
  - pause puis défilement : 2 × 24 s ;
  - rampe 20 ↔ 5 Mb/s : 2 × 20 s ;
  - clip de jeu : 2 × 20 s.
- Sorties dans `bench-out\d3d12v2\n95\refit`.

| contenu | mesure | `refit=0` (aujourd'hui) | `refit=1` |
|---|---|---|---|
| défilement | débit / cible, fenêtres de 2 s | 0,68 ; 0 / 56 à ±10 % | 0,83 ; 6 / 56 |
| | taille / budget : moy. / p95 | 0,72 / 2,03 | 0,89 / 2,07 |
| | images recodées ; recodées deux fois | 15,7 % ; — | 9,5 % ; 25 (0,4 %) |
| | très au-dessus (> 2,5 budgets) | 12 | 0 |
| pause puis défilement | débit / cible | 0,59 | 0,79 |
| | première image après l'arrêt : moy. / max | 0,61 / 1,71 budget | 0,69 / 1,60 |
| | très au-dessus | 2 | 0 |
| rampe | marches suivies en 3 images | 13 / 20 (montées : 3 / 10) | 18 / 20 (montées : 8 / 10) |
| | très au-dessus | 4 | 0 |
| clip de jeu | débit / cible ; taille / budget | 0,86 ; 0,93 | 0,86 ; 0,93 |
| toutes les passes | `host_total` moy. / p95 / p99 | 9,55 / 15,7 / 33,7 ms | 9,58 / 16,5 / 33,3 ms |

(« Très au-dessus » : le compte de l'encodeur, voir plus bas. Latence sur
12 600 images par bras.)

- Le rejeu du §8n.8 est confirmé : il prédisait 0,893 du budget par image
  sur le texte, le banc mesure 0,889.
- Sur le fil, le texte passe de 0,68 à 0,83 de la cible. Chaque image reste
  à 0,89 de son budget, et la capture coûte le reste : le N95 livre 56 à 57
  images sur 60, et 0,94 × 0,89 ≈ 0,84.
- Le critère ±10 % n'est toujours pas tenu sur ce N95 (6 fenêtres sur 56).
- Rampe : les montées sont suivies bien plus vite. À 5 Mb/s, le ré-encodage
  d'aujourd'hui pousse le QP jusqu'à 40-51, et après la montée il faut
  plusieurs images pour en redescendre. Avec `refit=1`, la phase basse
  finit vers QP 36-38. Les descentes étaient déjà suivies (10 sur 10 dans
  les deux bras).
- Le clip de jeu ne bouge pas : 0,5 % d'images recodées dans les deux bras.
- La latence ne bouge pas. Le coût, c'est un troisième encodage sur 0,4 %
  des images de texte (environ 6 ms de plus chacune sur le N95).

**Le compte des images très au-dessus.**
- `rate-report.py` en trouve 32 dans les passes de rampe `refit=1`,
  l'encodeur aucune.
- 5 sont des images de marche : codées juste avant une descente, elles
  portent déjà la nouvelle cible dans le CSV. Les passes `refit=0` en ont
  aussi.
- Les 27 autres sont dans une passe où Chrome a ralenti : les images sont
  arrivées à 33, puis 54 par seconde. La session donne alors à l'encodeur
  le budget des images qui arrivent vraiment (`EffectiveCadence`) : 36 363,
  puis 22 222 kb/s par seconde d'images, pour 20 000 sur le fil.
- L'outil, lui, divise toujours la cible du banc par 60. Ces images, à 2,5
  à 3,5 fois ce budget nominal, restaient sous 2,5 fois le budget réel.
- Le compte juste est donc celui de l'encodeur. ✅ Depuis `5d842941`,
  chaque image porte le débit que l'encodeur tenait (colonne
  `encoder_kbps`), et `rate-report.py` en fait le budget. Il donne à côté
  le compte contre la cible / 60 quand les deux diffèrent, et compte une
  marche à partir de la première image codée au nouveau débit.
- Ces ralentissements de Chrome touchent 4 passes `refit=1` et 1 passe
  `refit=0`. Les présentations par seconde diffèrent peu : 57,1 contre
  56,2 sur le défilement (`refit=0` puis `refit=1`), égales ou meilleures
  avec `refit=1` sur les autres contenus. Et `refit=1` recode moins
  d'images. Rien n'accuse l'encodeur, mais c'est à surveiller sur l'Arc.

**Conclusion.** Le correctif tient ce que le rejeu promettait : 22 % de
débit en plus sur le texte, plus aucune image très au-dessus, les marches
mieux suivies, le clip et la latence inchangés. → Décision §9-14 du plan :
l'activer par défaut après ses passes sur l'Arc.

### 8n.10 `refit=1` sur l'Arc, puis par défaut (27-28/09/2026)

**Montage.**
- Binaires de `5d842941` : la clé `refit=`, la sonde sans les adaptateurs
  d'écran virtuel (`a83087db`, trois GPU listés au lieu de quatre) et la
  colonne `encoder_kbps`.
- Même exécuteur élevé qu'au §8n.6 : classe REALTIME, HEVC 1080p60,
  20 Mb/s, `governor=0`, sur l'écran de l'Arc (contenu rendu par l'Arc).
- Mêmes passes qu'au §8n.9, alternées A-B puis B-A. Sorties dans
  `bench-out\d3d12v2\refit-arc`.

| contenu | mesure | `refit=0` | `refit=1` |
|---|---|---|---|
| défilement | débit / cible, fenêtres de 2 s | 0,88 ; 27 / 56 à ±10 % | 0,91 ; 43 / 56 |
| | taille / budget : moy. / p95 | 0,89 / 1,87 | 0,91 / 1,86 |
| | images recodées ; recodées deux fois | 3,1 % ; — | 3,8 % ; 42 (0,6 %) |
| | très au-dessus (> 2,5 budgets) | 8 | 0 |
| pause puis défilement | débit / cible | 0,67 | 0,78 |
| | première image après l'arrêt : moy. / max | 0,72 / 1,60 budget | 1,00 / 1,70 |
| | très au-dessus | 1 | 0 |
| rampe | marches suivies en 3 images | 18 / 20 (montées : 8 / 10) | 20 / 20 |
| | très au-dessus | 2 | 1 |
| clip de jeu | débit / cible ; taille / budget | 0,928 ; 0,933 | 0,931 ; 0,935 |
| toutes les passes | `host_total` moy. / p95 / p99 | 3,99 / 4,64 / 6,43 ms | 4,02 / 4,71 / 6,96 ms |

(Latence sur 13 400 images par bras. Les images arrivent à 60 par seconde
dans toutes les passes : le budget de l'encodeur n'est jamais mis à
l'échelle.)

- Même sens qu'au N95, en plus petit : l'Arc ne recodait que 3 % des images
  de texte, contre 16 % sur le N95. Le débit du texte gagne 3 %, mais les
  fenêtres dans ±10 % de la cible passent de 27 à 43 sur 56.
- « Très au-dessus » : le compte de `rate-report.py` est désormais celui de
  l'encodeur. Contre la cible / 60, il en trouverait 4 dans chaque bras de
  la rampe ; les 2 ou 3 de plus sont les images de marche, codées sous
  l'ancien débit, que `encoder_kbps` départage maintenant.
- Le coût : le troisième encodage (environ 4 ms de plus) sur 0,6 % des
  images de texte.
  - Au-delà de 8 ms : 57 images de texte au lieu de 19.
  - p99 de toutes les passes : 6,43 → 6,96 ms. Sur la pause seule : 6,47 →
    8,10 ms (16 images au-delà de 8 ms au lieu de 12, sur 1 432).
  - La moyenne ne bouge pas, le clip de jeu non plus.
- Ce que ça achète : une image très au-dessus pèse au moins 104 Ko à
  20 Mb/s. Sur un lien au débit de la cible, elle met au moins 42 ms à
  passer au lieu de 17, et retarde les suivantes. Sur un LAN à 1 Gb/s, elle
  passe en 1 ms : là, l'échange coûte +0,5 ms au p99 pour un texte un peu
  plus net.
- L'Arc recode un peu plus d'images avec `refit=1` (3,1 → 3,8 %), le N95
  moins (15,7 → 9,5 %). Une image recodée à deux budgets laisse le tampon
  plus plein, et les budgets suivants plus petits.

**Décision (Bruno, 28/09) : `refit=1` par défaut** (`0d09df8d`). `refit=0`
reste la clé de banc de l'« avant ».

### 8n.11 C8.1 : les pannes injectées, sur l'Arc (28/09/2026)

Depuis §9-23, D3D12 est le chemin par défaut d'Intel. Sa promesse : rien de
ce qu'il rate ne coupe un stream, la session repart en D3D11 sur une
keyframe et dit pourquoi. Ces chemins-là, aucun banc ne les prenait de
lui-même.

**Montage.**
- `MW_D3D12_FAULT=<panne>@N` dans l'environnement (C8.1) : la panne à la
  N-ième conversion de la chaîne, ou à la N-ième ouverture pour `open`.
  - `encode` : l'image est codée, puis jetée comme une erreur du pilote.
  - `convert` : la conversion n'est pas enregistrée, comme une liste
    refusée.
  - `timeout` : la conversion attend une fence que personne ne signale ;
    l'encodage dépasse le délai (3 s), puis la file est relâchée, comme un
    GPU coincé derrière un jeu.
  - `removed` : `ID3D12Device5::RemoveDevice` avant l'encodage, comme un
    TDR ou une mise à jour du pilote.
  - `open` : la chaîne ne s'ouvre pas.
- `--native-bench` sans clé de chaîne (donc D3D12 par la table, §9-23), sur
  l'écran de l'Arc qui fait défiler du texte : HEVC 1440p60, 20 Mb/s, 10 s,
  jeton limité, panne à la conversion 180 (vers 3 s).
- Le CSV porte désormais la chaîne de chaque image (colonne `pipeline`). Le
  résumé dit la bascule : après quelle image, combien de temps sans image,
  et si la première image d'après est une keyframe.
- Chaque flux est relu par ffmpeg (erreurs de décodage), et ffprobe compte
  les images décodées.

| panne | trou sans image | images D3D12 / D3D11 | keyframes (images) | erreurs de décodage |
|---|---|---|---|---|
| `encode@180` | 618 ms | 178 / 384 | 0, 178 | 0 |
| `convert@180` | 618 ms | 179 / 388 | 0, 179 | 0 |
| `timeout@180` | 3 592 ms | 178 / 210 | 0, 178 | 0 |
| `removed@180` | 640 ms | 178 / 383 | 0, 178 | 0 |
| `open@1` | — (D3D11 dès le départ) | 0 / 541 | 0 | 0 |
| `encode@180`, `intra=1` | 603 ms | 179 / 386 | 0, 179 | 0 |
| `removed@180`, `intra=1` | 623 ms | 179 / 382 | 0, 179 | 0 |

- Chaque fois, D3D11 reprend sur une keyframe, la seule en dehors de la
  première. Le journal dit la panne, puis « D3D12 lost (…) — back to D3D11
  for the rest of the session », puis la chaîne choisie et pourquoi.
  - `removed` : « the encode list was refused (0x80070057) (the device is
    gone: the D3D12 device was removed (0x887A0005)) ».
  - `open@1` : « D3D11 runs: the D3D12 build failed (fault injected …) ».
- Le trou de 0,6 s correspond à la duplication rouverte, puis à oneVPL ouvert
  en D3D11, jusqu'à la première image complète. Celui de `timeout` ajoute les
  3 s d'attente, le délai qui sépare un GPU perdu d'un GPU occupé (§3.1 du
  plan).
- Flux sans erreur de décodage, toutes les images décodées. ffmpeg ne relève
  que des DTS non croissants au changement d'encodeur : le nouvel encodeur
  reprend son POC à 0 à son IDR, et un flux brut n'a pas d'autre horloge.
  Le navigateur, lui, reconfigure son décodeur sur des paramètres changés à
  une keyframe (`VideoDecodeWorker.js`), comme après un changement de mode.
- `removed` n'a rien bloqué côté D3D11 : la duplication, qui attendait la
  fence B du périphérique retiré, a été rouverte sans attente.

**Le N95** (mw-intel, pilote 32.0.101.7088), mêmes pannes. Le banc tourne sur
l'écran de l'UHD (1920×1080 à 60 Hz), en HEVC 1080p60 à 20 Mb/s pendant 12 s,
avec le jeton limité et un exécuteur dans la session console :

| panne | trou sans image | images D3D12 / D3D11 | keyframes (images) | erreurs de décodage |
|---|---|---|---|---|
| `encode@180` | 1 396 ms | 179 / 398 | 0, 179 | 0 |
| `convert@180` | 1 080 ms | 179 / 416 | 0, 179 | 0 |
| `timeout@180` | 4 039 ms | 179 / 256 | 0, 179 | 0 |
| `removed@180` | 1 148 ms | 179 / 429 | 0, 179 | 0 |
| `open@1` | — (D3D11 dès le départ) | 0 / 641 | 0 | 0 |

Même comportement que sur l'Arc. Le trou est deux fois plus long : un N95 met
plus de temps à ouvrir oneVPL en D3D11. La ligne de session donne le pilote
(C8.3) : « on Intel(R) UHD Graphics (driver 32.0.101.7088) ».

### 8n.12 C8.3 bis : le retour à la duplication, sur l'Arc (28/09/2026)

Au §8n.4 (endurance), un worker non SYSTEM perdait la duplication derrière un
écran verrouillé, repassait sur WGC, donc en D3D11, et n'en revenait jamais.
Désormais, tant que WGC remplace une duplication refusée pour une raison qui
peut passer, la boucle guette son retour. Elle regarde toutes les 500 ms si le
bureau de l'utilisateur est revenu. Elle ouvre alors une duplication à côté,
en espaçant les essais de 1 à 30 s tant qu'ils échouent. Dès que l'une
s'ouvre, le redémarrage ordinaire reprend DDA, et la chaîne D3D12 avec.

**Montage.** Le même banc qu'au §8n.11, sur 12 s. `MW_DDA_REFUSE` tient lieu
d'écran verrouillé : la duplication est refusée comme le bureau sécurisé la
refuse (`0x80070005`). Avec `<à>+`, la duplication en cours est d'abord
perdue, comme un verrouillage la perd.

| `MW_DDA_REFUSE` | bascules (trou sans image) | images | keyframes (images) | erreurs de décodage |
|---|---|---|---|---|
| `3+4` | D3D12 → D3D11 sur WGC après l'image 167 (678 ms), D3D11 → D3D12 après l'image 381 (470 ms) | 441 D3D12, 214 D3D11 | 0, 168, 382 | 0 |
| `4` | départ sur WGC en D3D11, D3D11 → D3D12 après l'image 221 (369 ms) | 222 D3D11, 480 D3D12 | 0, 222 | 0 |

- Journal : « Desktop Duplication refused this display (… 0x80070005 …) —
  falling back to Windows.Graphics.Capture », puis la chaîne D3D11 et sa
  raison (« captured through Windows.Graphics.Capture, which hands its
  pictures to D3D11 only »), puis « Desktop Duplication serves this display
  again — leaving Windows.Graphics.Capture », puis D3D12.
- D3D12 revient à 0,4 s près de la fin du refus, comme après un
  déverrouillage réel. Sans SYSTEM, rien ne s'essaie tant que le bureau
  sécurisé est là, donc rien ne s'espace.
- Le premier essai de ce banc espaçait aussi les refus simulés (1, 2 puis
  4 s) : D3D12 revenait 3,6 s après la fin du refus. La simulation suit
  désormais le vrai bureau sécurisé.
- Sur WGC, `host_total` monte à 9,0-9,7 ms en moyenne, contre 6,6-7,5 en D3D12
  sur la duplication, pour le même contenu (images capturées, hors keyframes).
- **N95**, `3+4` : D3D12 → D3D11 sur WGC après l'image 124 (1 520 ms), puis
  D3D11 → D3D12 après l'image 202 (493 ms). 388 images en D3D12 et 78 en
  D3D11, keyframes aux images 0, 125 et 203, aucune erreur de décodage. Sur
  WGC, le N95 ne suit que ~20 i/s en D3D11.

### 8n.13 C8.2 : les scénarios, sur l'Arc (28/09/2026)

Le VDD de Bruno est rendu par l'Arc et activé pour l'occasion (§8n.5 : jamais
le mode d'un écran physique). `--native-bench` le capture pendant qu'un script
change son mode et son HDR, puis remet tout en place. Chaîne prise par `auto`,
HEVC 1080p60 à 20 Mb/s, jeton limité.

| scénario | ce qui change en plein stream | images | redémarrages de capture | keyframes | erreurs de décodage |
|---|---|---|---|---|---|
| session SDR, 50 s | 1920×1080 à 60 Hz, puis à 120 Hz, 1440×1080 (4:3), retour en 2560×1440, HDR activé, HDR coupé | 678, toutes en D3D12 | 7 | 7 | 0 |
| session HDR (`hdr=1`), 30 s | HDR coupé, puis réactivé | 1 755, toutes en D3D12 | 3 | 3 | 0 |
| deux sessions, 30 s | l'écran physique de l'Arc et le VDD, deux processus en même temps | 1 091 + 1 312, toutes en D3D12 | — | 1 + 1 | 0 |

- Chaque redémarrage reconstruit la chaîne D3D12. Aucun ne repasse en
  D3D11, pas même quand le bureau devient FP16 sous une session SDR : la
  conversion D3D12 fait le tone mapping.
- Le 4:3 ramène l'image à 1440×1080, la taille de l'écran. Revenu à
  2560×1440, l'écran ne la fait pas regrandir : c'est le comportement d'avant
  (`frameForDisplay`), la session ne suit la forme qu'avec
  `followDisplayShape`.
- La session HDR qui voit l'écran quitter le HDR se reconstruit en SDR
  (« the display left HDR — rebuilding on the SDR path »), toujours en D3D12.
  Au retour du HDR, elle reste en SDR avec tone mapping : ce que le client a
  négocié ne change pas en cours de route.
- Deux sessions sur l'Arc : deux périphériques D3D12, un par processus, comme
  deux workers. 36 et 44 i/s, avec 4,7 et 5,1 ms de moyenne de la
  présentation à l'image encodée. L'Arc rend en plus deux pages de défilement
  en 1440p, dont une à 120 Hz.
- Le premier essai des deux sessions n'avait qu'un écran en mouvement : le
  lancement de la page au profil `.chrome-bench` tue aussi celle au profil
  `.chrome-bench2` (filtre `*.chrome-bench*` de `kiosk.ps1`). Il faut lancer
  le profil par défaut d'abord.
- À la désactivation du VDD, l'agencement est revenu à l'identique : DISPLAY1
  principal, 59,95 et 60 Hz, HDR coupé partout, `vdd_settings.xml` intact.

**30 min sous RE9.** RE9 tourne sur l'Arc (la copie propre, réglages légers
de G2, scène de la pluie, 3D à 99 %). `--native-bench` capture l'écran de
l'Arc pendant 1 800 s, en HEVC 1080p60, jeton limité (classe HIGH). La
mémoire du processus et sa VRAM sont relevées toutes les 10 s.
- 42 053 images, toutes en D3D12, une seule keyframe : ni perte, ni repli, ni
  image recodée. RE9 rend ~23 i/s, et chacune de ses images est encodée.
- Mémoire plate de la 1re à la 29e minute : privée 221,3 à 221,5 Mo, VRAM de
  l'Arc au processus 49,7 Mo et mémoire partagée 61,5 Mo sans un octet de
  plus, 408 à 414 handles, 10 à 13 threads.
- 24,3 ms de moyenne de la présentation à l'image encodée, 36,9 au p99 : en
  classe HIGH, les files D3D12 attendent le jeu (§8n.2). Le worker installé,
  en REALTIME, faisait 5,7 ms sous RE9 (§8n.7). Ce banc mesure la tenue, pas
  la latence.
- RE9 remis comme avant : préférence GPU d'origine (la RTX), `config.ini` à
  son empreinte.

**Le N95** (VDD rendu par l'UHD, 2560×1440 à 120 Hz ; mêmes scripts à
distance) :
- Session SDR, 50 s : 1 140 images, toutes en D3D12, sept redémarrages de
  capture, six keyframes, aucune erreur de décodage. Le 1440×1080 est refusé
  par le VDD du N95 (`ChangeDisplaySettingsEx` −2, mode absent) : trois modes
  au lieu de quatre, HDR activé puis coupé compris.
- Deux sessions (l'écran de l'UHD et le VDD) : toutes deux en D3D12 jusqu'au
  bout, aucune erreur. Mais l'UHD rend aussi les deux pages de défilement,
  dont une en 1440p à 120 Hz : 7,6 et 10,4 images/s, 125 et 66 ms de moyenne
  avec des pointes à 2,6 s. C'est la limite du N95, pas une panne.
- ⚠️ **Session HDR : le périphérique D3D12 est perdu dès la première image**
  (`0x887A0006`, DXGI_ERROR_DEVICE_HUNG). La session repasse en D3D11 et
  continue en HDR, sans erreur de décodage : le repli a joué sur une vraie
  panne. Trois passes courtes l'ont reproduit à chaque fois, en bilinéaire,
  en 1:1 et en Lanczos-2 : la mise à l'échelle n'y est pour rien.
  `video_encode12` encode du Main 10 sur l'UHD à partir d'images P010 copiées
  depuis le CPU (106/106) : l'encodeur n'y est pour rien non plus. Reste le
  rendu de la conversion dans les plans du P010 (suite au §8n.14).

### 8n.14 Le HDR du N95 : un effacement qui perdait le GPU (28/09/2026)

**Le test.** `color_convert12_gpu`, nouveau, rejoue sur chaque vrai GPU (et
plus seulement sur WARP) les étapes de la première image d'une session :
l'effacement de la sortie au noir, puis la conversion, 1440p FP16 → 1080p
codé en 1088. Chaque étape attend sa fence. `MW_TEST_CONVERT12_STEP=clear|draw`
fait tourner une étape P010 seule, dans son propre processus, parce qu'un GPU
perdu emporte le périphérique D3D12 du processus.

| sur l'UHD du N95 (pilote 32.0.101.7088) | NV12 | P010 |
|---|---|---|
| effacement (`ClearRenderTargetView` sur les deux plans) | passe | **périphérique perdu** (`0x887A0006`), deux fois sur deux |
| conversion seule, sans effacement | passe | passe |

L'Arc, l'iGPU AMD et la RTX passent tout. **La cause :**
`ClearRenderTargetView` sur une vue d'un plan de P010 (R16 ou R16G16). Le
dessin dans ces mêmes vues passe. La première image de chaque session HDR
efface la bande sous l'image, et l'image noire d'un écran verrouillé fait de
même.

**Le correctif.** Le noir est dessiné au lieu d'être effacé : `PsFill`, un
point d'entrée du HLSL commun qui rend la valeur des constantes, et un PSO par
plan. Il couvre la bande de la première image et l'image noire
(`recordClearBlack`), en NV12 comme en P010. Les octets sont les mêmes : sur
WARP, la conversion D3D12 reste identique à celle de D3D11, noirs 8 bits et
P010 compris, bande noire comprise.

**Vérifié sur le N95** : `color_convert12_gpu` passe en entier (5/5), et une
vraie session HDR tient en D3D12 sur 15 s sur le VDD en HDR. Elle donne 262
images, une keyframe, un flux Main 10 PQ BT.2020 que ffmpeg décode sans
erreur.

### 8n.15 C8.4 : l'écran verrouillé, par Bruno, et le worker SYSTEM qui ne servait pas (28/09/2026)

**Montage.** Édition dev de la branche installée sur DualRTX
(`0.3.1-c08939c9-dev`, réglage `d3d12`). Bruno streame depuis son Mac, par
Internet (`stream.dev`) : l'écran de l'Arc, puis un stream HDR sur l'écran
virtuel de l'édition dev.

**Premier essai.**
- Le verrouillage (Démarrer → avatar → Verrouiller) s'affiche dans le stream,
  et la souris s'y déplace. Mais sur l'écran du PIN, la souris disparaît et le
  clavier ne tape rien.
- Le stream HDR et l'overlay (« D3D12 VE (Intel) », de retour après chaque
  bascule) sont justes.
- Le journal donne la cause : chaque session refuse le worker SYSTEM (« pipe
  client is "" (pid N), not this executable ») et prend la tâche élevée
  (« this user, elevated (task) »). Ce worker a la classe REALTIME, mais sur
  `Winlogon` la duplication lui est refusée (`0x80070005`, passage par WGC et
  D3D11 comme C8.3 bis), et `SendInput` aussi (erreur 5).
- Les journaux de la prod montrent la même chose depuis le 26/09 : **le niveau
  2 du §31 du design n'a jamais servi en v0.3.1**. Le serveur, non élevé, ne
  peut pas lire l'image d'un processus SYSTEM laissé à la DACL par défaut de son
  jeton, et le contrôle d'image échoue.

**Correctif `f1e8e8e3`.** Le service crée le worker avec une DACL à lui : pour
l'utilisateur qui l'a demandé, lecture de l'image, attente et arrêt, rien de
plus.

**Second essai (`0.3.1-f1e8e8e3-dev`)** : **le déverrouillage marche**. Le
journal :
- « Worker spawned through its launcher as "SYSTEM (launcher service)" » et
  « GPU scheduling class REALTIME (token SYSTEM) » ;
- « input: running as SYSTEM — following the desktop switch » ;
- au verrouillage, « now on the "Winlogon" desktop », une duplication rouverte
  sur `Winlogon` et « D3D12 Video Encode ready » ;
- au retour, « now on the "Default" desktop », une nouvelle duplication, D3D12
  encore. Pas de WGC, pas de D3D11, pas d'erreur.

**Trouvé en lisant ce journal.** `keyboard_debug`, posé à la main dans les
réglages dev et prod de ce poste pour le chantier clavier, écrivait chaque
touche tapée en stream, et donc le PIN, dans le journal du worker. Avec
l'accord de Bruno, le réglage est coupé dans les deux éditions, et les 114 828
lignes `[KBD]` de 327 journaux (dev et prod) sont effacées sur place, à longueur
égale. Le correctif `1887b2ea` empêche que ça recommence : les diagnostics
clavier se taisent tant que l'entrée n'est pas sur le bureau de l'utilisateur,
quoi que dise le réglage.

### 8n.16 §9-15 : le scaler matériel d'Intel, par D3D12 Video Process (28/09/2026)

**La question.** Sur le N95, sous une charge 3D qui sature l'iGPU, la
conversion attend le jeu alors que l'encodeur VE, sur le moteur vidéo, ne
l'attend pas (§8n.8). Le scaler du moteur vidéo (SFC), que D3D12 Video Process
expose, sortirait-il la conversion de la file du jeu ? La condition de Bruno :
une route SFC seulement si la sonde ne voit plus le jeu.

**La sonde.** `mw-d3d12-lab queues` gagne deux variantes :
- `vp` : le bureau BGRA réduit et converti en NV12 BT.709 limité par
  `ProcessFrames`, sur une file VIDEO_PROCESS ;
- `vp-pointer` : la même, avec le pointeur composé en second flux (alpha par
  pixel).

Les temps sont mesurés comme pour les autres variantes. La dernière image est
comparée à celle des shaders : PSNR par plan et moyennes des plans. Avec
`--picture`, la source est un vrai bureau : une page de texte rendue par
Chrome, ou une image du clip de jeu du banc. La campagne passe par
`d3d12-lab-campaign.ps1 -Set vp`, avec l'exécuteur élevé (REALTIME), la
charge `mw-gpu-load` au niveau 1,05 et 60 soumissions par seconde. Les shaders
en bilinéaire, le filtre que le garde-fou du produit choisit sur le N95, sont
passés à part (`queues --filter bilinear`). Sorties dans
`bench-out\d3d12v2\n95\vpp`.

**Ce que l'UHD offre** (l'Arc répond pareil) :
- BGRA → NV12 de 8×8 à 8192×8192, 16 flux d'entrée, fusion alpha ;
- **pas de HDR** : FP16 → P010 PQ et FP16 → NV12 tone-mappé sont refusés ;
- les horodatages de la file VIDEO_PROCESS n'encadrent pas le travail du
  scaler (0,03 ms pour 4 ms de mur) : seul le temps mur compte ;
- la couche de validation veut la destination d'un `ResolveQueryData` en
  `COPY_DEST` sur cette file.

**Temps mur (ms, moyenne / p99), classe REALTIME, files GLOBAL_REALTIME.**

| 1440p → 1080p | repos | sous charge | jeu sous charge (i/s) |
|---|---|---|---|
| shaders, Lanczos-2 | 12,9 / 14,4 | 22,1 / 32,3 | 20,8 |
| shaders, bilinéaire (le choix du produit ici) | 2,9 / 3,3 | 10,9 / 20,7 | 37,9 |
| Video Process | 4,0 / 4,7 | 11,2 / 21,6 | 42,4 |
| Video Process + pointeur | 6,9 / 7,2 | 11,6 / 23,5 | 37,8 |

| 1080p → 1080p (l'écran du N95) | repos | sous charge | jeu sous charge (i/s) |
|---|---|---|---|
| shaders | 2,3 / 5,2 | 9,3 / 17,5 | 39,6 |
| Video Process | 2,5 / 2,9 | 10,4 / 21,3 | 42,8 |

- Sous charge, la file VIDEO_PROCESS attend le jeu autant que la file DIRECT :
  environ 10 ms avant que le travail démarre, contre 8 à 9 ms pour les
  shaders, et ce même en GLOBAL_REALTIME. Le scaler ne sort pas la conversion
  de la file du jeu.
- Il rend quelques images au jeu (42 i/s au lieu de 38 à 40) : ses
  millisecondes ne sont plus prises au moteur 3D. Mais avec le pointeur en
  second flux, la composition repasse par le moteur 3D (2,4 à 5,9 ms de GPU
  horodatées, jeu à 37,8 i/s).
- Au repos, il est plus lent que les shaders : 4,0 contre 2,9 ms en 1440p →
  1080p. Sur l'Arc, 3,7 à 4,0 ms contre 2,2.

**Qualité, sur les vraies images** (1440p → 1080p, contre notre Lanczos-2) :
- texte : 29,9 dB, là où notre bilinéaire est à 28,7. Un peu plus sombre
  (moyenne de Y 211 contre 213), parce qu'il réduit en gamma et non en lumière
  linéaire ;
- jeu : 56,6 dB, comme le bilinéaire (57,8) ;
- sans mise à l'échelle, il donne la même image à un code près (83 dB sur
  l'Arc) ;
- le pointeur composé est juste, sauf sa partie en inversion (XOR), que le
  scaler ne sait pas faire.

**Verdict (§9-15 du plan) : pas de route SFC.** La condition n'est pas
remplie : sous charge, le scaler attend le jeu comme les shaders, et sa
latence n'est pas meilleure (11,2 contre 10,9 ms en moyenne). Son seul gain,
quelques images par seconde rendues au jeu, disparaît dès que le pointeur est
composé. Et il coûterait le HDR et le pointeur en inversion.

### 8n.17 Les deux dernières passes de la phase 8 : la bascule dans Chrome, 30 min en REALTIME (28/09/2026)

Build de `main` juste après la fusion de la branche (`1db193d9`), sur DualRTX.
L'écran de l'Arc est le principal ce jour-là (celui de la RTX a quitté le
bureau), celui de l'AMD est à sa droite.

**La bascule D3D12 → D3D11 vue par un vrai navigateur (C8.1).** Le banc du
§8n.11 relit le flux avec ffmpeg. Un navigateur, lui, doit reconfigurer son
décodeur quand un autre encodeur reprend le flux sur sa keyframe.
- Montage : une instance `--dev` du build, avec `MW_D3D12_FAULT=encode@900`
  dans son environnement. Son worker en hérite : une `--dev` n'a ni service ni
  tâche à son nom, et son worker part en enfant simple. L'écran de l'Arc fait
  défiler du texte. Le client est un Chrome dédié sur l'écran de l'AMD, rendu
  par l'AMD, jamais par le GPU qui encode. HEVC 4:2:0 1080p60, débit auto (20
  Mb/s au plus). L'overlay est relevé par CDP toutes les 4 s
  (`c81-stream.ps1`).
- La session part en D3D12 (conversion DIRECT, puis D3D12 Video Encode). La
  panne tombe sur la 900e image, 15 s après le départ : « D3D12 lost (…) —
  back to D3D11 » à 18:37:49,374, oneVPL prêt en D3D11 à 18:37:49,982. Soit
  0,6 s, comme au banc.
- Le client ne s'arrête pas : 60 i/s avant, 45 sur la fenêtre de relevé qui
  contient la bascule, 60 après. Le flux reste en HEVC d'un bout à l'autre,
  vers 18 Mb/s, avec une latence affichée de 7,2 à 10 ms. Aucune erreur de
  décodage, aucune demande de keyframe, rien dans la console de la page.
- Au moment de céder, la chaîne D3D12 fait son bilan : 900 images à QP 31,2 en
  moyenne, aucune très au-dessus de son budget, 24 recodées.
- Le premier essai est à jeter. Le Chrome de banc demandait le 4:4:4, que
  l'AMD ne décode pas en HEVC, et la négociation a fini en H.264 4:2:0. La
  route D3D12 ne prend pas encore le H.264 : D3D11 dès le départ, donc pas de
  bascule à voir. Relancé en HEVC 4:2:0.
- En passant : revenue en D3D11, la session lâche le Lanczos-2 dix secondes
  plus tard (« conversion + encode took 13 ms a frame against a 16 ms
  interval ») et continue en bilinéaire. La chaîne D3D12 l'avait gardé pendant
  ses 15 s, sur le même contenu.

**30 min sous RE9, en classe REALTIME (C8.2).** Le §8n.13 a tenu 30 min en
classe HIGH, celle d'un worker au jeton limité. Restait la classe du worker
installé, REALTIME.
- Montage : RE9 est lancé sans élévation, comme un joueur le lance (copie
  propre, réglages légers, scène de la pluie, 3D de l'Arc à 99,8 %). Seul
  `--native-bench` est élevé, par un exécuteur à commande fixe
  (`c82rt-bench.ps1`) : « GPU scheduling class REALTIME (token elevated) »,
  conversion sur la file DIRECT en GLOBAL_REALTIME. Chaîne prise par `auto`
  (D3D12), HEVC 1080p60 à 20 Mb/s, 1 800 s. La mémoire et la VRAM du processus
  sont relevées toutes les 10 s.
- 40 131 images, toutes en D3D12, une seule keyframe : ni perte ni repli, et
  une seule image recodée. RE9 rend ~22 i/s, et chacune de ses images est
  encodée. ffmpeg relit les 40 131 paquets sans une erreur (`-err_detect
  crccheck+bitstream+buffer`), et les images tirées toutes les 5 min montrent
  la scène, justes.
- Présentation → image encodée : 6,69 ms de moyenne, 9,22 au p95, 10,24 au p99,
  contre 24,25, 30,72 et 36,86 en classe HIGH. La classe REALTIME sort la
  chaîne de la file du jeu, comme au §8n.7. D'une tranche de 5 min à l'autre,
  la moyenne ne bouge pas (6,65 à 6,78 ms).
- 138 images sur 40 128 (0,3 %) dépassent 16,7 ms. Quatre dépassent 80 ms (85
  à 115 ms), toujours dans l'encodage. Le maximum, 262 ms, est la première
  image : l'attente de la première capture.
- Mémoire plate : privée de 220,2 à 220,7 Mo du début à la fin, VRAM au
  processus 49,6 Mo et mémoire partagée 61,5 Mo sans un octet de plus, 387 à
  394 handles, 10 à 16 threads.
- RE9 remis comme avant : préférence GPU d'origine (la RTX), `config.ini` à
  son empreinte.

### 8n.18 G4 : NVENC et AMF en entrée D3D12, face à leur chemin D3D11 (28/09/2026)

La porte G4 du plan (§5) : une route D3D12 dont l'encodeur est le SDK du
fabricant (phase 7) reste candidate si elle fait au moins aussi bien que
D3D11. En moyenne de `host_total`, pas pire ; au p99, pas plus de 5 % pire ;
et mieux de 10 % ou de 0,5 ms sur l'un des deux. Au repos, pas plus de 0,2 ms
pire.

**Montage.**
- DualRTX. L'écran de la RTX 5060 Ti est le principal (0,0) ; ceux de l'iGPU
  AMD (« AMD Radeon(TM) Graphics ») et de l'Arc sont à sa droite.
- Build `559db975` (C7.4), dans `bench-out\d3d12v2\g4-bin`.
- Passes de 12 s, alternées entre les bras, HEVC 1080p à 20 Mb/s, en classe
  REALTIME : un exécuteur élevé à commande fixe (`g4-runner.ps1`) ne lance que
  `ab-native-bench.ps1`.
- Bras : `d3d11`, `nvenc12` ou `amf12` (le SDK en D3D12, `strict12=1`) et
  `ve12` (D3D12 Video Encode, pour mémoire).
- Charge : RE9 (copie propre, lancé sans élévation, 3D de la RTX à 96 %).
  Repos : texte qui défile.

**Résultats** (moyenne / p99 de `host_total`, en ms).

| Cas | d3d11 | SDK en D3D12 | ve12 |
|---|---|---|---|
| RTX sous RE9, 1080p60 (4 tours) | 1,87 / 2,57 | nvenc12 2,12 / 3,17 | 5,36 / 6,07 |
| RTX sous RE9, 1080p120 (4 tours) | 1,83 / 2,60 | nvenc12 2,06 / 3,02 | — |
| RTX au repos, 1080p60 (8 tours) | 2,08 / 3,38 | nvenc12 2,41 / 4,12 | — |
| iGPU AMD au repos, 1080p60 (8 tours) | 8,64 / 14,62 | amf12 9,80 / 18,80 | 15,41 / 20,87 |
| iGPU AMD au repos, 1080p120 (8 tours) | 8,51 / 14,65 | amf12 9,24 / 18,29 | 16,60 / 22,56 |

- Aucune route ne passe. NVENC en D3D12 coûte 0,2 à 0,3 ms de plus que son
  chemin D3D11 et 16 à 24 % au p99, sous le jeu comme au repos. AMF en D3D12
  coûte 0,7 à 1,2 ms et 4 ms au p99.
- Les débits d'images tiennent (60 et 122 i/s sur la RTX, 116 sur l'AMD), sauf
  VE sur l'AMD à 120 i/s (80 i/s). Le jeu ne perd rien : 269 i/s en D3D11, 283
  avec nvenc12, 271 avec ve12.
- Pourquoi. D3D11 ne perdait déjà rien sur ces deux GPU (G2) : NVENC et AMF y
  encodent sur leur moteur, et la conversion passe devant le jeu dès la classe
  REALTIME. En D3D12, NVENC ne synchronise rien lui-même : il faut des fences
  en entrée et en sortie, et une attente CPU avant de verrouiller le flux.
  AMF en D3D12 veut une file pont et rend l'image dans un état qu'il faut
  remettre sur le GPU. Ces allers-retours sont le surcoût mesuré.
- Les soaks de 30 min ne sont pas lancés : ils ne servent qu'à une route
  candidate.

**Verdict.** Les lignes NVIDIA et AMD de la table restent D3D11. Les routes
D3D12 par SDK restent joignables par le réglage et par `enc12=` au banc :
NVENC est l'encodeur D3D12 des GPU NVIDIA (G1), AMF celui qu'on essaie à la
main sur AMD. Sur l'iGPU AMD, AMF en D3D12 bat VE de 5 à 7 ms : si un jour la
route D3D12 devait servir sur AMD, ce serait par AMF.

### 8n.19 C9.1 : le H.264 par D3D12 Video Encode (28/09/2026)

Build `69c8ea53`. VE code désormais le H.264 comme le HEVC : profil High, une
image gardée, fenêtre glissante, POC de type 2.

**Tests matériels** (`mw-native-tests video_encode12`, les trois GPU).
- 40 images, keyframe forcée à la 30e, perte de la 18e annoncée à la 20e :
  l'IDR suit, faute d'image plus ancienne gardée. La garde relit les en-têtes
  de tranche du pilote avec nos SPS et PPS, sans écart.
- La RTX et l'Arc codent en CABAC avec la transformée 8×8. L'iGPU AMD n'a pas
  la 8×8 : le PPS le dit.
- ffmpeg relit les trois flux sans une erreur (High, niveau 4.2,
  1920×1080 recadré depuis 1088). Chaque image est plus proche de sa propre
  entrée que de ses voisines, d'au moins 11,6 dB (`align-psnr.py`).

**Le chemin produit** (`--native-bench`, `codec=h264,pipeline=d3d12,enc12=ve`,
texte qui défile, 8 s, 20 Mb/s).

| GPU | Images | host_total moy. / p99 (ms) | Encodage moy. (ms) |
|---|---|---|---|
| RTX 5060 Ti | 481 | 2,18 / 3,54 | 1,82 |
| iGPU AMD | 475 (59 i/s) | 14,89 / 29,05 | 12,84 |
| Arc A380 | 481 | 5,34 / 12,63 | 5,03 |

Une seule keyframe par flux, zéro erreur au décodage, et les images tirées
montrent le texte net. Pas de porte pour autant : en Auto, la ligne D3D12
d'Intel ne vaut que pour le HEVC (`autoD3d12Codec`), et le H.264 reste en
D3D11 tant qu'un banc ne l'a pas mesuré.

### 8n.20 C9.2 : l'AV1 par D3D12 Video Encode (28/09/2026)

Build `e623f62c`. En AV1, le pilote code la tuile et rien d'autre. Le
délimiteur, l'en-tête de séquence et l'en-tête de trame sont écrits par nous,
une fois l'image codée, avec les valeurs que le pilote a choisies.

**Ce que disent les pilotes** (`mw-d3d12-lab caps`, étendu à l'AV1).
- RTX 5060 Ti : exige la restauration de boucle, CDEF et la segmentation
  automatique. Il rend après l'image le quantificateur, les deltas, le filtre
  de boucle, CDEF, le mode composé et la trame primaire. Une tuile suffit
  jusqu'en 4K.
- Arc A380 : n'exige rien et ne déclare aucune valeur rendue après l'image.
  Il les remplit pourtant toutes (qindex de sa régulation, filtre 5/5/4/4,
  CDEF sur 3 bits), comme Mesa les lit.
- iGPU AMD : pas d'encodeur AV1 en D3D12.

**Deux pièges, trouvés en route.**
- La RTX refuse la liste d'encodage (`E_INVALIDARG`). La couche de
  validation (`MW_D3D12_DEBUG=1`) le dit : une fonction exigée, ici la
  segmentation automatique, doit aussi être allumée sur chaque image.
- L'Arc écrit son AV1 au début du tampon, quel que soit le décalage de
  début de trame. En HEVC et en H.264, il le respecte. Lue au décalage, la
  tuile était du vide suivi de restes, et dav1d rejetait tout après l'image
  clé. Le diagnostic est venu d'une sonde hors ligne. Elle réemballe la
  tuile sous 128 variantes d'en-tête, et aucune ne décodait : l'en-tête était
  hors de cause, restait l'emplacement. La tuile est désormais prise au début
  du tampon, puis déplacée derrière les en-têtes.

**Résultats.**
- Tests matériels : 40 images, une image clé forcée, une perte réparée par
  une image clé. dav1d relit les deux flux sans une erreur, et chaque image
  est plus proche de sa propre entrée que de ses voisines, d'au moins
  12,7 dB. Les en-têtes de séquence acceptés sont figés en test.
- Chemin produit (`codec=av1,pipeline=d3d12,enc12=ve`, texte qui défile) :

| GPU | Images | host_total moy. / p99 (ms) | Encodage moy. (ms) | qindex moyen |
|---|---|---|---|---|
| RTX 5060 Ti | 481 | 1,91 / 2,59 | 1,57 | 150 |
| Arc A380 | 477 | 10,24 / 22,48 | 9,33 | 126 |

L'AV1 de l'A380 est le plus lent de ses trois codecs en D3D12 : 9,3 ms
d'encodage, contre 5,0 en H.264 au §8n.19. En Auto, rien ne change :
`autoD3d12Codec` ne vaut que pour le HEVC.

### 8n.21 G3 : la RTX en témoin du contrôle de débit maison (28/09/2026)

Le contrôle de débit maison a été mis au point sur l'Arc (§8n.6 à §8n.10). La
RTX, qui sait changer de débit en cours de séquence, le fait tourner en
témoin (`rc12=qp`) : il s'agit de savoir s'il tient sur l'encodeur d'un autre
fabricant, sans rien changer à son réglage.

**Montage.**
- DualRTX, écran de la RTX 5060 Ti (le principal), contenu rendu par la RTX
  dans le Chrome de banc en kiosque.
- Build `e623f62c`, figé dans `bench-out\d3d12v2\g3rtx-bin`.
- Exécuteur élevé à commande fixe (`g3rtx-runner.ps1`, celui du §8n.7 posé
  sur l'écran de la RTX) : classe REALTIME, HEVC 1080p, 20 Mb/s,
  `governor=0`, passes de 20 s.
- Trois bras :
  - notre contrôle de débit sur D3D12 Video Encode (`enc12=ve,rc12=qp`) ;
  - le débit du pilote par la même voie (`rc12=driver` : CBR, VBV,
    changement de débit à l'image suivante) ;
  - NVENC par D3D11, la voie par défaut des GPU NVIDIA.
- Sorties dans `bench-out\d3d12v2\g3rtx`, lues par `rate-report.py`
  (`report.txt`).

| contenu | bras | fenêtres à ±10 % | taille / budget : moy. / p95 | très au-dessus | `host_total` moy. / p99 (ms) |
|---|---|---|---|---|---|
| défilement 60 i/s (2 passes) | notre débit | 7 / 9 et 7 / 9 (0,78 à 0,97) | 0,92 / 1,35 | 0 | 8,34 / 9,12 et 8,17 / 9,53 |
| | débit du pilote | 6 / 9 et 6 / 9 (0,61 à 0,94) | 0,86 / 1,03 | 0 | 8,12 / 9,28 et 8,11 / 8,74 |
| | NVENC (D3D11) | 7 / 9 et 7 / 9 (0,76 à 0,94) | 0,90 / 0,98 | 0 | 2,18 / 2,81 et 2,16 / 3,13 |
| clip de jeu 60 i/s | notre débit | 7 / 9 (0,89 à 0,96) | 0,93 / 1,23 | 0 | 7,88 / 9,21 |
| | débit du pilote | 0 / 9 (0,80 à 0,89) | 0,84 / 0,95 | 0 | 7,74 / 8,77 |
| | NVENC (D3D11) | 4 / 9 (0,83 à 0,95) | 0,89 / 0,96 | 0 | 2,12 / 2,64 |
| défilement 120 i/s | notre débit | 7 / 9 (0,82 à 0,96) | 0,93 / 1,96 | 0 | 6,68 / 12,14 |
| | débit du pilote | 7 / 9 (0,79 à 1,00) | 0,96 / 1,51 | 2 | 6,59 / 8,28 |
| | NVENC (D3D11) | 7 / 9 (0,80 à 0,94) | 0,91 / 1,06 | 0 | 1,87 / 2,44 |

- Rampe 20 ↔ 5 Mb/s : notre débit suit 8 marches sur 9 en 3 images (p95
  1,63, aucune image très au-dessus). Le pilote en suit 7 sur 10 : il met
  27 images à une montée et 8 à une descente, et laisse passer 2 images très
  au-dessus. NVENC les suit toutes en une image.
- Pertes (`lose=45`) : 5 sur 5 réparées par invalidation, sans image clé ;
  7 fenêtres sur 9, p95 1,33.
- Pause puis défilement : la première image après l'arrêt fait au plus 1,2
  budget (0,1 à 1,2 sur 5 arrêts).
- Écran fixe : QP 18 atteint, puis 38 renvois de 2,5 Ko à QP 18.
- Le pilote code le QP demandé sur chaque image : aucun écart sur les
  9 passes.
- Images recodées : 2 à 6 par passe à 60 i/s (0,2 à 0,5 %), 47 sur 2 393
  à 120 i/s (2 %), 11 sur la rampe, aux marches descendantes.
- Première image clé : 92 à 100 Ko avec notre débit, 316 Ko avec le CBR du
  pilote (7,6 budgets).

**Lecture.**
- Notre contrôle tient G3 sur un second fabricant, réglé sur l'Arc et
  inchangé. Il fait mieux que le débit du pilote NVIDIA par la même voie :
  sur le clip (7 fenêtres sur 9 contre 0), sur les marches, et sans image
  très au-dessus. Le pilote colle plus près du budget au p95, mais reste
  sous la cible (0,84 à 0,86 en moyenne).
- NVENC par D3D11 reste la référence : p95 au plus 1,06 et marches suivies en
  une image, pour 2 ms d'hôte. La voie par défaut de NVIDIA ne change pas.
- VE encode une image en 7,4 à 8,0 ms sur une RTX au repos à 60 i/s, 6,2 ms
  à 120 i/s, quand le GPU tourne plus vite (5,4 ms de `host_total` sous RE9
  au §8n.18) ; NVENC en 1,7 à 1,9 ms. En moyenne, notre débit coûte autant
  que celui du pilote. Au p99, il coûte plus là où il recode : 12,1 contre
  8,3 ms à 120 i/s, 15,6 contre 9,4 ms sur la rampe. Chaque ré-encodage est
  un encodage VE de plus.

**Critères G3 sur la RTX.**
- Débit à ±10 % : 7 fenêtres sur 9 dans chaque passe ; les ratés sont sous
  la cible (0,78 à 0,89).
- p95 ≤ 2 × budget : tenu (1,23 à 1,96).
- Marches suivies en 3 images : 8 sur 9.
- Écran fixe à QP 18 : tenu.
- Latence : égale au débit du pilote en moyenne. Au p99, +3,9 ms à 120 i/s
  et +6,3 ms sur la rampe : le ré-encodage sur un encodeur lent.

**Reste pour G3** : le profil « Internet » sur un vrai stream (pertes, RTT,
marches de bande passante, gouverneur actif), puis le test de Bruno.

### 8n.22 G3 : le profil « Internet », sur un vrai stream (28/09/2026)

Jusqu'ici, G3 s'est jugé au banc, sans récepteur (`governor=0`). Ici, un vrai
client reçoit le stream à travers un lien dégradé. Le gouverneur agit, le
relais jette ce que le lien ne vide pas, et le client demande ses
réparations.

**Montage.**
- Hôte : l'édition MoonlightWebDev installée sur DualRTX (`f1e8e8e3`, celle
  que Bruno teste), écran de l'Arc avec du texte qui défile en kiosque, HEVC
  1080p60. Le client fixe 20 Mb/s.
- Client : Chrome 154 sur l'UM790Pro (Ubuntu 24.04, décodage HEVC matériel
  par VA-API), piloté par DevTools. Transport `webrtc-dc-udp` : le canal de
  données SCTP. ICE a choisi une paire IPv6.
- Lien : `tc netem` sur l'UM790Pro, appliqué à l'UDP des ports média de
  l'hôte (48550-48573), en IPv4 comme en IPv6. Le lien descendant passe par
  `ifb0` (débit, pertes, délai), le lien montant garde le même délai.
  Profil :

| phase | durée | lien descendant | pertes | aller-retour ajouté |
|---|---|---|---|---|
| LAN | 20 s | — | — | — |
| Internet | 30 s | 30 Mb/s | 0,3 % | 40 ms |
| étroit | 30 s | 8 Mb/s | 0,3 % | 40 ms |
| large | 30 s | 30 Mb/s | 0,3 % | 40 ms |
| pertes | 30 s | 30 Mb/s | 2 % | 40 ms |
| Internet | 30 s | 30 Mb/s | 0,3 % | 40 ms |
| LAN | 15 s | — | — | — |

- Deux passes, même profil : D3D12 (le réglage de l'édition), puis D3D11
  (réglage passé à `d3d11` le temps de la passe, puis remis). L'overlay est
  relu chaque seconde. Sorties et outils dans `bench-out\d3d12v2\inet` (hors
  dépôt).

**Résultats** (médianes par phase : images/s, débit reçu, latence de bout en
bout médiane / p90, `host_total`, file du lien médiane / max).

| phase | D3D12 | D3D11 |
|---|---|---|
| LAN | 60 i/s, 18,0 Mb/s, 8,8 / 10,6 ms, hôte 3,9 ms, file 2 / 4 ms | 47 i/s, 16,4 Mb/s, 28,8 / 31,4 ms, hôte 23,9 ms, file 2 / 5 ms |
| Internet | 60 i/s, 13,3 Mb/s, 95 / 538 ms, hôte 4,1 ms, file 21 / 364 ms | 47 i/s, 12,1 Mb/s, 162 / 613 ms, hôte 22,7 ms, file 23 / 522 ms |
| étroit | 60 i/s, 9,4 Mb/s, 99 / 223 ms, hôte 4,1 ms, file 3 / 321 ms | 48 i/s, 8,6 Mb/s, 149 / 454 ms, hôte 22,5 ms, file 20 / 525 ms |
| large | 60 i/s, 7,7 Mb/s, 31 / 126 ms, hôte 4,1 ms, file 1 / 113 ms | 48 i/s, 7,2 Mb/s, 239 / 625 ms, hôte 22,7 ms, file 8 / 565 ms |
| pertes | 53 i/s, 6,7 Mb/s, 689 / 856 ms, hôte 4,0 ms, file 466 / 588 ms | 40 i/s, 6,4 Mb/s, 784 / 913 ms, hôte 22,7 ms, file 577 / 772 ms |
| Internet | 60 i/s, 6,0 Mb/s, 191 / 524 ms, hôte 4,1 ms, file 2 / 627 ms | 48 i/s, 5,8 Mb/s, 132 / 738 ms, hôte 22,8 ms, file 10 / 675 ms |
| LAN | 60 i/s, 5,8 Mb/s, 8,4 / 57 ms, hôte 4,1 ms | 48 i/s, 5,6 Mb/s, 30 / 146 ms, hôte 22,2 ms |
| gels du lien (nombre · plus long) | 4 · 2,4 s | 9 · 2,4 s |
| images perdues (réseau) | 1,42 % | 1,69 % |

**Ce que disent les journaux de l'hôte.**
- À l'arrivée des pertes (0,3 %, 40 ms d'aller-retour), la file SCTP monte
  à 250 ms, le relais jette des deltas (« link not draining »), un gel de
  2,4 s suit, et le gouverneur descend de 20 à 4 Mb/s en 4 à 5 s. Même chose
  sur les deux chaînes.
- Avec 0,3 % de pertes au hasard, le débit reste à 4-6 Mb/s, même quand le
  lien offre 30 Mb/s. C'est le plafond d'un transport qui lit chaque perte
  comme une congestion : la formule de Mathis donne 5,3 Mb/s pour 0,3 % de
  pertes à 40 ms d'aller-retour. Le gouverneur ne remonte qu'une fois les
  pertes finies. Au dernier LAN, D3D12 revient à 16,4 Mb/s en 25 s, D3D11 est
  à 8,7 Mb/s à la fin de la passe.
- D3D12 : plus de 100 pertes réparées par un delta (invalidation) ; 3
  réparées par une image clé, là où la perte était plus ancienne que l'image
  gardée. Notre débit : 11 730 images, QP 35,8 en moyenne, 1 328 recodées
  (11 %, dont 44 deux fois), 46 très au-dessus (0,4 %), aucun écart de QP du
  pilote. À 4 Mb/s, le budget par image n'est que de 8 Ko, et le texte qui
  défile le dépasse souvent.
- D3D11 : l'Arc ne tient pas 60 i/s dès le départ (« frames arrive at 48
  fps »), l'encodage oneVPL montant à ~19 ms (la lenteur du §8n.7). Pendant
  les pertes, le relais jette 5 images clés, que le lien ne vide pas, et une
  invalidation refusée devient une image clé.

**Lecture.**
- Notre contrôle de débit suit le gouverneur dans un vrai stream, dans les
  deux sens, et l'hôte reste à 4 ms dans toutes les phases. D3D12 fait mieux
  que D3D11 partout : images/s, latence de l'hôte, gels (4 contre 9), et
  latence de bout en bout médiane dans 6 phases sur 7.
- Ce qui se sent par Internet est le transport. Dès 0,3 % de pertes, le
  canal de données plafonne vers 5 Mb/s, sa file monte à 0,5-0,8 s pendant
  les pertes, et l'arrivée des pertes coûte un gel de 2,4 s. D3D11 a les
  mêmes, et D3D12 n'y change rien : c'est le plafond du canal de données
  sous pertes, désormais mesuré, que le chantier du transport devra lever.

**Critères G3, sur le vrai stream.**
- Débit : suit le gouverneur, à la baisse (4 s) comme à la hausse.
- Latence pas pire qu'en D3D11 : tenu, et de loin côté hôte.
- Pas de pompage visible : **tenu**. Test de Bruno le 29/09, depuis son
  téléphone, en 5G, par le rendez-vous de l'édition dev. L'écran de l'Arc,
  donc D3D12 Video Encode avec notre contrôle de débit, a été regardé à 10
  puis 5 Mb/s fixes, sur deux contenus :
  - la page de texte qui défile ;
  - RE9 (la copie propre, réglages légers), une rue sous la pluie.

  Aucun pompage vu : la netteté ne « respire » pas. G3 est tenue en entier.

**Pièges du montage**, pour qui le refait.
- Un profil Chrome neuf, sous une session ouverte automatiquement, attend le
  trousseau GNOME (verrouillé) pour ses cookies, et aucune page ne se charge.
  Il faut lancer Chrome avec `--password-store=basic`.
- ICE prend l'IPv6 s'il en trouve : un filtre sur l'IPv4 de l'hôte ne bride
  rien. Le script compte les paquets passés par `netem` avant de mesurer.

### 8n.23 Phase 10 : deux images en vol (`pipelined=1`, 29/09/2026)

La chaîne D3D12 encode une image à la fois : le fil de capture attend le
bitstream avant d'acquérir la présentation suivante. Avec `pipelined=1`,
l'encodage et la remise à l'envoi passent sur un fil à eux, et la capture
convertit l'image suivante dans une seconde sortie. Il y a au plus deux images
en vol : une image convertie qui attend encore quand une plus récente arrive
est jetée, jamais mise en file (`ee2c326e`, design §32.17).

**Montage.**
- Binaire figé `bench-out\d3d12v2\c10-bin` (`ee2c326e`). Sur DualRTX,
  exécuteur élevé (classe REALTIME, `c10-runner.ps1`) ; sur le N95, la copie
  de l'exécuteur du §8n.8 (`n95-runner-c10.ps1`).
- A/B alterné `ab-native-bench.ps1` : D3D12 Video Encode une image à la fois
  (`ve12`, la référence) contre la même chaîne en `pipelined=1` (`pipe12`).
  HEVC 1080p, 20 Mbit/s, page de défilement, ou RE9 (copie propre, réglages
  légers du §8n.7) sur l'écran de l'Arc.
- Sous charge, sur le N95 : `mw-gpu-load` 1,05, huit passes seules de 15 s,
  alternées.
- Sorties et scripts : `bench-out\d3d12v2\c10`.

| cas (tours) | i/s | hôte, moyenne | hôte, p99 | encodées pendant un encodage / jetées, par passe |
|---|---|---|---|---|
| iGPU AMD, repos, 1080p120 (6) | 79,4 → **94,5** | 16,50 → **15,87** ms | 22,58 → **20,02** | ~1 300 / 175-250 |
| iGPU AMD, repos, 1080p60 (4) | 59,8 → 59,9 | 15,37 → 14,81 | 20,72 → 19,96 | ~710 / 0-1 |
| RTX (VE), repos, 1080p120 (4) | 119,6 → 119,9 | 6,55 → 6,59 | 9,13 → 8,93 | 14-81 / 0 |
| Arc, repos, 1080p120 (4) | 116,6 → 116,8 | 3,99 → 4,04 | 6,55 → 6,48 | 10-17 / 1-3 |
| Arc sous RE9, 1080p60 (4) | 24,9 → 25,4 | 5,90 → 5,61 | 26,4 → 25,8 | 0-1 / 0 |
| Arc sous RE9, 1080p120 (4) | 25,0 → 24,9 | 4,74 → 4,72 | 27,0 → 22,2 | 0 / 0 |
| N95, repos, écran 1080p60 (4) | 55,9 → 57,4 | 8,31 → 8,03 | 38,9 → 25,6 | 2-8 / 0 |
| N95, repos, écran virtuel 1440p120 → 1080p120 (4) | 40,7 → 41,0 | 26,5 → **28,5** | 84,7 → **136,4** | ~155 / 0 |
| N95 sous charge 3D, 1080p60 (4 + 4) | ~30 → ~30 | 30,9 → 30,7 | 60,4 → 59,0 | 1-4 / 0 |

- **L'iGPU AMD à 120 i/s est le cas du plan**. Son encodeur VE prend 12 ms
  par image, plus que l'intervalle : une image à la fois, la boucle n'en tient
  que 79 par seconde. Pipelinée, elle en tient 94,5, et l'hôte descend en même
  temps (−0,6 ms en moyenne, −2,6 ms au p99). La présentation qui attendait
  que la boucle revienne est désormais convertie à l'instant. Une image sur
  cinq est jetée pour une plus récente, comme prévu.
- **Là où l'encodage tient dans l'intervalle, rien ne change** : RTX et Arc
  au repos, et l'Arc sous RE9, où le jeu (25 i/s) ne présente pas plus vite
  que la chaîne n'encode.
- **Le N95 à 120 Hz perd** : même cadence, mais +2 ms en moyenne et +52 ms au
  p99. Son iGPU est saturé : les 155 conversions faites pendant un encodage
  disputent le GPU à l'encodeur au lieu d'occuper un moteur libre. Rien n'est
  jeté. À 60 Hz, le N95 gagne un peu (p99 38,9 → 25,6 ms, +1,5 i/s) ; sous
  une charge 3D, l'écart est nul.
- Le banc de fumée (classe HIGH, `c10\smoke`) a passé `lose=`, `ramp=`, un
  écran fixe et une panne injectée (`encode@200`) : chacun se comporte comme
  une image à la fois, D3D11 reprend sur une image clé, et ffmpeg décode
  chaque flux sans erreur.

**Lecture.** Le parallélisme ne paie que là où l'encodage dépasse
l'intervalle et où la conversion tourne sur un moteur que l'encodeur
n'occupe pas. Aujourd'hui, c'est l'iGPU AMD en VE à 120 i/s, une route qui
n'est pas celle d'AMD (AMF en D3D11, G4). Sur Intel, la chaîne par défaut,
c'est neutre à 60 i/s et nuisible sur un iGPU saturé. La clé reste une clé de
banc, et le défaut ne change pas (§9-26 du plan).

### 8n.24 Phase 11 : le temps GPU de l'encodeur, l'encodeur gardé, les cartes de QP (29/09/2026)

Trois mesures de la phase 11, sur DualRTX en classe HIGH (sorties
`bench-out\d3d12v2\c11`).

**Le temps GPU de D3D12 Video Encode (C11.2, `de0184b2`).**
- `gputiming=1` remplit enfin la colonne `gpu_encode_us` : deux horodatages
  encadrent chaque soumission sur la file d'encodage, re-encodages compris.
- Sur une page qui défile, en 1080p60 : la RTX (VE) passe 7,6 ms sur les
  8,1 ms de l'étape d'encodage, l'iGPU AMD 8,8 sur 13,1. Le reste est l'attente
  de la conversion et le réveil.
- **L'Arc écrit ses deux horodatages avant que l'image soit codée** : 20 µs
  pour une image de 4 ms. Le moteur compare la somme des 30 premières
  soumissions au temps que la CPU les a attendues, et ne rapporte rien quand
  elle en fait moins du dixième : le journal le dit, la colonne reste à −1.
- Les cartes de QP et de SATD de la même famille ne sont pas dans le SDK du
  build (26100) : elles viennent avec un Agility SDK plus récent, hors
  périmètre.

**L'encodeur gardé à travers un redémarrage de capture (C11.4, `df5187ef`).**
- `keep12=1` garde D3D12 Video Encode quand le flux qu'il code ne change pas
  (codec, taille, cadence, HDR, intra-refresh). Le flux repart sur une image
  clé dans les deux cas.
- Écran virtuel rendu par l'Arc, scénario du §8n.13 : quatre changements de
  mode, puis HDR allumé et éteint, dans une session SDR. Deux passes par bras,
  alternées, soit 12 redémarrages chacun :

| | trou avant l'image clé du redémarrage, médiane | moyenne |
|---|---|---|
| encodeur refait (aujourd'hui) | 717 ms | 729 ms |
| `keep12=1` | **546 ms** | **528 ms** |

- Aucune erreur de décodage, D3D12 d'un bout à l'autre. L'encodeur est gardé
  à 8 reconstructions sur 9 : à la première, la largeur du flux passe de 1668
  à 1440 et il est refait, comme prévu.
- ⚠️ Activer l'écran virtuel la nuit a fait perdre l'écran de la RTX, en
  veille : il n'est pas revenu en le désactivant, et l'écran de l'Arc est
  resté principal.

**Les cartes de QP sur les pilotes du jour (C11.3, étude).** Sonde
`mw-d3d12-lab encode --rc delta|absolute --delta-sweep 6` :

| GPU (pilote) | carte delta | carte absolue |
|---|---|---|
| RTX 5060 Ti | acceptée, cases de 32 px, 240 images sans erreur | acceptée, idem |
| Arc A380 (32.0.101.8993) | annoncée (16 px), **refusée à `EncodeFrame`** (`E_INVALIDARG`) | non annoncée |
| iGPU AMD (32.0.21045.5002) | non annoncée | annoncée (64 px), **refusée à `EncodeFrame`** (`E_INVALIDARG`) |

- Comme le 26/09 : seule la RTX applique une carte de QP, et son stream passe
  par NVENC en D3D11, qui a la sienne. Un ROI (pointeur, texte) dans la chaîne
  D3D12 n'a donc pas de GPU où servir aujourd'hui.

### 8n.25 C11.1 : où passe le temps du Lanczos-2 (29/09/2026)

Le rééchantillonnage du produit est fait de deux passes 1-D de Lanczos-2,
dilaté au rapport, en lumière linéaire. Chaque tap décode son texel sRGB par un
`pow()` et calcule son poids par deux `sin()`. Sur le N95, les deux passes
coûtent ~11 ms par image en 1440p → 1080p, contre les 1,5 ms que le garde
autorise : le N95 streame en bilinéaire.

Le plan prévoyait des shaders SM 6 en types 16 bits (C11.1). La sonde
`mw-d3d12-lab scale` mesure d'abord où va le temps. Elle écrit les mêmes passes
de quatre façons et compare chaque image à celle du produit :
- **product** : les shaders du produit, compilés comme lui (FXC, O3) ;
- **lut** : les poids calculés une fois par la CPU, une lecture par tap au lieu
  de deux `sin()` et d'une division ;
- **once** : `lut`, plus le bureau décodé en lumière linéaire une seule fois,
  dans une passe à part, au lieu d'une fois par tap ;
- **half** : `once`, avec une arithmétique `min16float` dans les deux passes.

Temps GPU par image, médiane de 200 images, en classe HIGH (sorties
`bench-out\d3d12v2\c11\scale`) :

| GPU | product | lut | once | half | bilinéaire (le repli) |
|---|---|---|---|---|---|
| N95 (UHD Graphics), 1440p → 1080p | 10,93 ms | 11,88 | 15,53 | 15,53 | **1,46** |
| N95, 1080p → 720p | 5,40 | 5,59 | 7,91 | 7,47 | 0,70 |
| iGPU AMD (2 CU) | 5,95 | **3,75** | **2,54** | 2,59 | 0,34 |
| Arc A380 | 0,94 | 0,99 | 1,39 | 1,38 | 0,14 |
| RTX 5060 Ti | 0,18 | 0,14 | 0,24 | 0,24 | 0,02 |

Chaque variante rend l'image du produit à une valeur près, au pire.

**Lecture.**
- **Sur Intel, le Lanczos-2 est lié à la mémoire, pas au calcul.** Retirer les
  `sin()` et les `pow()` ne change rien sur le N95 ni sur l'Arc. Ajouter une
  passe de décodage coûte ce qu'elle lit et écrit (+40 %). Le simple fetch
  bilinéaire prend déjà 1,46 ms sur le N95 : le budget entier. Aucun shader ne
  fera tenir un Lanczos-2 dans 1,5 ms sur ce GPU, et les types 16 bits visent
  le calcul, qui n'est pas le goulet.
- **Sur le petit iGPU AMD, c'est l'inverse** : les poids précalculés font
  −37 %, le décodage unique −57 %. Mais 2,5 ms restent au-dessus du budget, et
  le garde retirerait le Lanczos-2 là aussi.
- `min16float` ne gagne rien nulle part, et le SM 6 n'y changerait rien.

**Verdict C11.1** : pas de shaders SM 6 ni de DXC dans le build. Aucune des
variantes ne fait passer un GPU de l'autre côté du garde, et la meilleure
perdrait sur Intel, la chaîne par défaut. La sonde reste au labo pour le jour
où un GPU tombera près du seuil.

### 8n.26 Deux images en vol à 244 Hz, et les moteurs du N95 (29/09/2026)

Bruno a demandé deux vérifications avant de trancher `pipelined=1` (§9-26 du
plan) :
- un gros GPU à très haute fréquence y gagne-t-il ?
- sur Intel, l'encodeur partage-t-il l'unité de calcul de la conversion ? Si
  oui, une priorité pourrait les départager.

**Montage.**
- L'écran virtuel de Bruno (VDD by MTT), rendu tour à tour par la RTX puis par
  l'Arc, en 1920×1080, 2560×1440 et 3840×2160 à 244 Hz.
- La page de défilement dessus, et le flux à la taille de l'écran, donc sans
  rééchantillonnage.
- HEVC à 240 i/s, 20 Mbit/s, classe HIGH. A/B alterné `ab-native-bench.ps1`,
  4 tours par bras. Binaire figé `bench-out\d3d12v2\c26-bin`.
- Script `c26-hf.ps1` ; sorties `bench-out\d3d12v2\c26`.
- Trois bras :
  - `ve12` : D3D12 Video Encode, une image à la fois ;
  - `pipe12` : le même en `pipelined=1` ;
  - la chaîne D3D11 du fabricant en référence : NVENC pour la RTX, oneVPL
    pour l'Arc.

**RTX 5060 Ti** (i/s ; hôte, moyenne / p99 ; étape d'encodage) :

| mode | `ve12` | `pipe12` | NVENC D3D11 (défaut) |
|---|---|---|---|
| 1080p244 | 205,9 · 6,88 / 9,23 ms · 4,61 ms | 217,1 · 6,60 / 8,97 · 6,34 | **239,3 · 1,59 / 1,94 · 1,41** |
| 1440p244 | 123,8 · 10,09 / 13,21 · 7,75 | 128,5 · 9,92 / 12,90 · 9,65 | **237,5 · 2,84 / 3,52 · 2,64** |
| 2160p244 | 57,8 · 19,4 / 23,6 · 16,9 | 59,6 · 18,8 / 21,7 · 18,5 | **226,4 · 6,26 / 8,69 · 4,24** |

- Sur la route D3D12, deux images en vol font gagner 3 à 5 % d'images, et un
  peu d'hôte. L'encodeur VE de la RTX est trop lent pour 244 Hz dès le 1080p,
  en vol ou non.
- La chaîne par défaut de NVIDIA, NVENC en D3D11, suit 240 i/s jusqu'en 1440p
  avec 1,6 à 2,8 ms d'hôte. Elle n'a rien à gagner.
- **Seul le 4K à 244 Hz met NVENC en limite** : 4,24 ms par image pour un
  intervalle de 4,1 ms, d'où 226 i/s. Là, deux images en vol pourraient
  combler l'écart (~6 %). Il faudrait d'abord les écrire pour NVENC, qui
  n'enregistre qu'une entrée.

**Arc A380** (mêmes colonnes ; oneVPL D3D11 en référence, l'ancienne route
d'Intel) :

| mode | `ve12` (défaut d'Intel) | `pipe12` | oneVPL D3D11 |
|---|---|---|---|
| 1080p244 | 233,6 · 3,71 / 6,79 ms · 3,14 ms | **238,5** · 3,83 / 7,19 · 3,60 | 237,0 · 3,37 / 5,22 · 3,02 |
| 1440p244 | 220,9 · 4,66 / 9,61 · 3,85 | **230,8** · 4,73 / 9,93 · 4,47 | 220,1 · 5,55 / 9,64 · 4,18 |

- L'Arc, qui a sa propre mémoire, gagne 2 à 4,5 % d'images en deux images en
  vol à 244 Hz. L'hôte bouge à peine : +0,1 ms en moyenne, +0,3 à +0,4 ms au
  p99.
- C'est l'inverse du N95 à 120 Hz (§8n.23), dont la mémoire est partagée.
- **Décision de Bruno, sur ces chiffres** : deux images en vol par défaut sur
  un GPU Intel à mémoire propre, une à la fois ailleurs (`9814c606`, design
  §32.20).

**Les moteurs du N95** (sonde `mw-d3d12-lab encode`, les huit images NV12 du
labo codées en boucle ; compteurs Windows « GPU Engine » du processus, lus par
WMI) :

| cas | moteur 3D | moteur vidéo (« Video Decode ») | images |
|---|---|---|---|
| encodage seul, 1080p60 | **0 %** | 26 % | 1 128 en 20 s |
| conversion + encodage, 1080p60 | 18 % | 36 % | 1 171 |
| encodage seul, 1080p120 | **0 %** | 60 % | 2 210 |
| conversion 1440p → 1080p + encodage, 120 i/s demandées | 79 % | 15 % | 1 052 |

- **L'encodeur d'Intel ne touche pas aux shaders.** En encodage seul, le moteur
  3D reste à 0 %. Tout passe par le moteur vidéo, que Windows appelle « Video
  Decode » sur Intel : le même bloc code et décode.
- Il n'y a donc pas d'unité partagée qu'une priorité pourrait départager.
  Conversion et encodage tournent sur deux moteurs distincts, et une priorité
  de file ne départage que le travail d'un même moteur.
- Ce qu'ils partagent, c'est la mémoire, à un seul canal, commune avec le CPU
  (le N95 y est lié, §8n.25). Quand la conversion tourne, le même encodage
  à 60 i/s occupe le moteur vidéo 36 % du temps au lieu de 26 %, soit ~40 %
  plus long par image.
- En 1440p → 1080p, la conversion sature le moteur 3D à elle seule (79 %).
  C'est la perte du N95 à 120 Hz du §8n.23 : deux images en vol ne créent pas
  de capacité, elles font attendre l'encodeur derrière la mémoire.

**keep12 par défaut, vérifié.** Même scénario que le §8n.24, sur l'écran
virtuel rendu par l'Arc : quatre changements de mode, puis HDR allumé et
éteint. Le binaire du jour, sans aucune clé :
- le journal dit « D3D12 Video Encode kept across 6 rebuild(s) (keep12) » ;
- l'encodeur est refait une fois, quand la largeur du flux passe de 1668 à 1440 ;
- trous avant l'image clé : 371 à 660 ms, médiane 471 ms (546 avec `keep12=1`
  cette nuit, 717 sans) ;
- 0 erreur de décodage, D3D12 de bout en bout (`c11\keep\c927-default`).

### 8n.27 L'image jetée au calage du lien, nommée à l'encodeur : client Linux (§9-25, 29/09/2026)

`namedrops=1` (design §9.10.2) contre le comportement d'avant, sur un vrai
stream que le lien fait caler.

**Montage.**
- **Hôte** : l'édition dev `0.3.1-81c55309-dev` installée sur DualRTX, donc le
  worker SYSTEM. Le bras est posé avant chaque passe dans le `native_tuning` du
  `settings.json`. La chaîne est nommée dans les deux bras (`pipeline=`), parce
  que le réglage de l'édition était resté à `d3d12` :
  - RTX : NVENC en D3D11 ;
  - iGPU AMD : AMF en D3D11 ;
  - Arc : D3D12 Video Encode (sa chaîne par défaut), et oneVPL en D3D11 (l'ancienne).
- **Client** : le Chrome de l'UM790Pro (Ubuntu 24.04, décodage VA-API), qui fait
  le pari du ride-out. HEVC 1080p60 à 20 Mbit/s.
- **Lien** : `tc netem` sur le client, sur l'UDP du stream seulement. Chaque passe
  enchaîne :
  - 12 s de calme ;
  - deux coupures totales (100 % de pertes dans les deux sens) de 0,3 s, deux
    de 0,5 s et deux de 1 s, toutes au-delà des 250 ms de tolérance du relais ;
  - deux bridages du lien descendant à 4 Mbit/s pendant 5 s.
- Bras alternés (off/on, puis on/off), 2 tours, 16 passes.
- Sorties : `bench-out\d3d12v2\c925b`. Un premier tour sans le détecteur est dans
  `c925`.
- Outils : `c925b-campaign.ps1`, `drops_run_band.py`, `c925_summary.py`, copiés
  dans `c925b\tools`.

**Le détecteur de dégâts.**
- La page de défilement porte son numéro d'image (`scroll.html?band=1`) : 24 bits
  en haut à gauche, les mêmes en bas à gauche, chacun au-dessus de son
  complément.
- Un échantillonneur injecté dans la page lit les deux bandes sur le canvas du
  stream, à chaque image d'animation.
- **Une image abîmée** a une bande cassée (un bit égal à son complément), ou deux
  numéros différents en haut et en bas. C'est ce que laisse une image prédite
  d'une référence que le client n'a jamais reçue, jusqu'au passage de la vague
  d'intra-refresh.
- Le même échantillonneur mesure les trous entre deux images nouvelles (les
  gels).
- **0 faux positif** : aucune image abîmée pendant le calme d'aucune passe, soit
  ~650 images par passe.

**Résultats** (sommes des 2 tours ; `c925b\summary.txt`) :

| Encodeur | Images abîmées, off → on | Gel total, off → on | Deltas jetés (nommés) | Deltas bloqués en attente d'une image clé |
|---|---|---|---|---|
| RTX, NVENC D3D11 | 1 107 → **77** (−93 %) | 16,1 → 15,8 s | 783 → 822 (822) | 54 → 61 |
| iGPU AMD, AMF D3D11 | 2 246 → **89** (−96 %) | 19,0 → 19,1 s | 1 342 → 516 (516) | 55 → 733 |
| Arc, D3D12 VE | 54 → **39** (−28 %) | 19,3 → 17,9 s | 23 → 171 (171) | 871 → 681 |
| Arc, oneVPL D3D11 | 1 244 → — | — | 908 → — | **le stream meurt, 3 fois sur 3** |

- **Les dégâts viennent surtout des bridages.** Un lien qui ne porte plus le
  stream pendant 5 s fait jeter delta sur delta. Sans nom, chacun laisse une
  image fausse jusqu'à la vague. Sur la RTX, sans `namedrops` : 248 et 646
  images abîmées pendant les bridages des deux passes ; avec : 32 et 0.
- **Les gels ne bougent pas**, ni leur nombre ni le plus long (~2,2 s, la coupure
  de 1 s et sa reprise). `namedrops` ne change pas ce que le lien laisse passer.
  Il change ce que l'image montre après.
- **D3D12 VE fait déjà peu de dégâts** : sans le pari du ride-out côté hôte, le
  relais y bloque les deltas jusqu'à une image clé (871 deltas bloqués). Nommer
  les images en retire un peu (681), et le gel total baisse de 8 %.
- **AMF bloque plus de deltas avec `namedrops`** (55 → 733). Il refuse certaines
  invalidations, et la session demande alors une image clé. Le gel total ne
  change pas.

**oneVPL se bloque.** Les trois passes « on » de l'Arc en oneVPL finissent en
stream mort :
- le relais nomme des images jetées d'affilée : 1072, 1073, 1074, 1075 ;
- oneVPL répare chacune depuis la même référence longue (« healed frame 1072
  with a delta against frame 1071 », etc.) ;
- puis un encodage ne se termine jamais : « waiting for the encoded frame
  failed: still executing », et la session finit 10 s plus tard (100 attentes
  de 100 ms).
- Le blocage est venu après la 4ᵉ réparation enchaînée dans une passe. Une autre
  passe avait d'abord tenu deux chaînes de 5, puis a bloqué après une chaîne
  de 7. La troisième est morte presque tout de suite, au bout de 648 images.
- **Pas reproduit hors ligne.** La clé de banc `lose=<n>x<rafale>` a été ajoutée
  pour cela : toutes les n images, une rafale d'images perdues d'affilée, chacune
  signalée dès sa sortie, comme le fait le relais. Sur l'Arc, oneVPL,
  intra-refresh, 30 s, aucun blocage avec `120x4`, `120x8` ni `60x20` (jusqu'à
  160 réparations). Ce qui le déclenche tient donc au chemin réel, pas à la seule
  suite des invalidations.
- **Jamais vu en production** : aucune trace dans les journaux des éditions de
  DualRTX sur 30 jours, alors que la v0.3.1 encode l'Arc en oneVPL, et que le
  client y nomme ses trous par le même chemin. Seules les rafales de `namedrops`
  l'ont produit.

**Trouvé en montant le banc** : `native_tuning` n'atteignait pas le worker
SYSTEM. Il le lisait dans son propre AppData, celui de `systemprofile`, et les
deux bras auraient été identiques. Corrigé par `29d5f8e7` : le serveur le lit et
le passe au worker.

### 8n.28 L'image jetée nommée : clients Windows et Mac (§9-25, 29/09/2026)

La suite du §8n.27, sur les deux autres clients choisis par Bruno.

**Montage.**
- **Hôte** : le même qu'au §8n.27 (édition dev `0.3.1-81c55309-dev`, bras posé
  dans le `native_tuning`, chaîne nommée dans les deux bras). oneVPL est laissé
  de côté : il se bloque avec `namedrops`.
- **Clients** :
  - Windows : le Chrome 154 du N95 (Wi-Fi, décodage matériel de l'UHD
    Graphics). Il fait le pari du ride-out, comme le client Linux.
  - Mac : le Chrome 154 du Mac M1 (Wi-Fi, VideoToolbox). Il ne le fait pas
    (`50b2b574`) : sans `namedrops`, l'hôte bloque tous les deltas après un
    delta jeté, jusqu'à une image clé demandée au dégorgement.
  - Le Mac est resté verrouillé, capot fermé. Son Chrome y rythme quand même
    ses images à 120 Hz, et le détecteur lit le canvas, pas l'écran.
- **Lien** : bridé **côté hôte**, puisque ni Windows ni macOS n'ont `netem`. Un
  petit brideur posé sur DualRTX (`mwshaper.py`, sur le pilote WinDivert 2.2.2,
  élevé) refait les coupures et les bridages du §8n.27. Il agit sur l'UDP des
  ports média (48550-48573), pour les seules adresses du client et du routeur.
- Chaque Chrome est piloté depuis DualRTX par son port DevTools, relayé par
  `ssh -L`. Événements et relevés sont datés par la seule horloge de DualRTX.
- Mêmes passes, même détecteur, bras alternés, 2 tours : 12 passes par client.
- Sorties : `bench-out\d3d12v2\c925w` (N95) et `c925m` (Mac). Outils dans
  `c925w\tools`.

**Relevé en montant le banc.**
- **Le premier chemin ICE passe en épingle par le routeur** (paire
  `prflx 82.67.150.202:48550` → `prflx 192.168.1.254`), avant de basculer sur le
  chemin direct. L'adresse du routeur est donc bridée aussi. Le brideur compte à
  part tout paquet d'une autre adresse : 0 dans toutes les passes.
- **Le démarrage du stream abîme des images sur le N95** : toutes, pendant ~4 s,
  dans la passe d'essai. Le compte ne commence donc qu'après 3 s sans dégât.
  Ensuite, 0 image abîmée au calme dans les 24 passes.
- **Sur le Wi-Fi du N95, le contrôle de débit de l'hôte descend à ~4 Mbit/s dès
  le démarrage** (« link: delay rising »), puis oscille entre 4 et 16 Mbit/s.
  Sous Linux (Ethernet) comme sur le Mac, il reste entre 15 et 20. Sur le N95,
  un bridage à 4 Mbit/s fait donc peu caler le lien : les dégâts y viennent
  surtout des coupures.

**Windows (N95)** (sommes des 2 tours ; `c925w\summary.txt` et `events.txt`) :

| Encodeur | Images abîmées, off → on | Gel total, off → on | Deltas jetés (nommés) | Deltas bloqués en attente d'une image clé |
|---|---|---|---|---|
| RTX, NVENC D3D11 | 687 → **100** (−85 %) | 15,0 → 15,0 s | 522 → 441 (441) | 98 → 111 |
| iGPU AMD, AMF D3D11 | 96 → **52** (−46 %) | 12,7 → 14,2 s | 380 → 120 (120) | 125 → 399 |
| Arc, D3D12 VE | 39 → 37 | 13,8 → 15,6 s | 25 → 171 (171) | 680 → 563 |

- Sur la RTX, les coupures de 0,3 et 0,5 s laissaient 144 et 322 images abîmées
  sans `namedrops`, 19 et 54 avec.
- AMF abîme bien moins ici que sous Linux (96 images contre 2 246) : ses dégâts
  y venaient des bridages, qui ne font plus caler le lien.
- Les gels ne bougent pas au-delà du bruit : deux passes du même bras
  s'écartent jusqu'à 1,9 s.

**Mac** (sommes des 2 tours ; `c925m\summary.txt` et `events.txt`) :

| Encodeur | Gel total, off → on | Deltas jetés (nommés) | Deltas bloqués en attente d'une image clé | Images clés demandées par le relais |
|---|---|---|---|---|
| RTX, NVENC D3D11 | 16,3 → 15,7 s | 37 → 1 066 (1 066) | 1 217 → **73** | 46 → **22** |
| iGPU AMD, AMF D3D11 | 20,9 → 21,8 s | 33 → 220 (220) | 1 094 → 914 | 41 → 31 |
| Arc, D3D12 VE | 23,4 → 24,1 s | 29 → 259 (259) | 1 124 → 911 | 37 → 33 |

- **Aucune image abîmée**, dans aucun bras (0 à 4 par passe) : VideoToolbox
  n'affiche pas une image fausse, il s'arrête.
- **Les gels ne changent pas** : de −3 à +4 %, moins que l'écart entre deux
  passes du même bras (jusqu'à 1,9 s).
- Sur NVENC, `namedrops` remplace l'attente d'une image clé par des images
  nommées : 1 217 deltas bloqués → 73, 46 images clés → 22. L'écran ne le voit
  pas. En LAN à 20 Mbit/s, une image clé passe en quelques dizaines de ms, et le
  gel suit la coupure elle-même, puis la reprise de SCTP.
- AMF et D3D12 VE bloquent encore beaucoup de deltas avec `namedrops` (914 et
  911) : ils refusent certaines invalidations (3 par passe), et la session
  demande alors une image clé.
- Les erreurs du décodeur (4 à 5 par passe) suivent chaque coupure d'au moins
  0,5 s, à l'identique dans les deux bras : `namedrops` n'y est pour rien.

**Ce qu'on en retient**, avec le §8n.27 :
- Sur les clients qui font le pari du ride-out (Chrome sous Linux et Windows),
  `namedrops` retire l'essentiel des images abîmées sur NVENC (−85 à −93 %) et
  AMF (−46 à −96 %), sans changer les gels.
- D3D12 VE n'y gagne presque rien : il attendait déjà des images clés.
- Sur un client Apple, il est neutre : il n'y a pas d'image abîmée à retirer, et
  l'image clé qu'il évite coûte peu en LAN.
- oneVPL se bloque avec lui (§8n.27) : jamais pour oneVPL.

**Décision de Bruno (29/09)** : `namedrops` par défaut pour NVENC et AMF en
D3D11, les deux encodeurs où le banc montre un gain (design §9.10.2). D3D12
Video Encode et oneVPL restent sans, comme NVENC et AMF en entrée D3D12, que ce
banc n'a pas mesurés.

Vérifié le jour même sur l'édition dev qui porte le défaut, par de courts
streams depuis le Chrome du N95. La ligne « streaming » du worker porte
`[link drops named]` sur NVENC et AMF en D3D11. Elle ne la porte pas sur
oneVPL, sur D3D12 VE, ni sur NVENC en entrée D3D12, le réglage de cette
édition. `namedrops=0` l'éteint sur NVENC comme sur AMF, et `namedrops=1`
l'allume sur NVENC en entrée D3D12. L'écran physique de la RTX ayant quitté le
bureau à 12:33, NVENC a streamé l'écran virtuel, rendu par la RTX le temps du
contrôle.

### 8n.29 L'image jetée nommée : NVENC et AMF en entrée D3D12 (§9-25, 29/09/2026)

La suite du §8n.28, sur les deux encodeurs que la décision de Bruno laissait
de côté faute de mesure : NVENC et AMF en entrée D3D12 (`NvencEncoder12`,
`AmfEncoder12`).

**Montage.**
- Le même qu'au §8n.28 : l'édition dev (`0.3.1-babff5ff-nd-dev`), le Chrome du
  N95, le brideur WinDivert côté hôte, les mêmes coupures et bridages, le même
  détecteur de dégâts.
- La chaîne est nommée par les clés de banc dans les deux bras :
  `pipeline=d3d12,enc12=nvenc` sur la RTX, `pipeline=d3d12,enc12=amf` sur
  l'iGPU AMD, avec `namedrops=0` ou `namedrops=1`. Le réglage de l'édition n'a
  pas bougé.
- L'écran physique de la RTX avait quitté le bureau : NVENC a streamé l'écran
  virtuel de Bruno, rendu par la RTX et mis en 2560×1440 pour le détecteur.
- Chaque passe relit dans le journal du worker l'écran, la chaîne, l'encodeur
  et le bras (`[link drops named]`) : les 8 passes sont conformes.
- Bras alternés, 2 tours : 8 passes. Sorties et outils :
  `bench-out\d3d12v2\c925d`.

**Relevé en montant le banc : un PID revient.** Le journal d'un worker est
nommé d'après son PID. Un worker qui reçoit le PID d'un ancien ajoute ses
lignes au fichier de celui-ci, qui garde sa date de création. La campagne
cherchait le journal par cette date et a manqué la passe 2 (un fichier ouvert
à 09:42). Elle prend maintenant les fichiers écrits depuis le début de la
passe, coupés à son heure.

**Résultats** (sommes des 2 tours ; `c925d\summary.txt` et `events.txt`) :

| Encodeur | Images abîmées, off → on | Gel total, off → on | Deltas jetés (nommés) | Deltas bloqués en attente d'une image clé |
|---|---|---|---|---|
| RTX, NVENC en entrée D3D12 | 810 → **107** (−87 %) | 20,3 → 20,6 s | 727 → 681 (681) | 203 → 179 |
| iGPU AMD, AMF en entrée D3D12 | 101 → 115 | 19,4 → 22,2 s | 673 → 131 (131) | 225 → 782 |

- **NVENC en entrée D3D12 fait comme en D3D11** (−85 % au §8n.28, même
  client). Les coupures de 0,3 et 0,5 s laissaient 190 et 257 images abîmées,
  10 et 35 avec ; les bridages 268, puis 1.
- **AMF en entrée D3D12 ne gagne rien sur ce client.** Ses dégâts restent
  faibles dans les deux bras, de 46 à 60 images par passe. Comme AMF en D3D11,
  il bloque plus de deltas en attente d'une image clé avec `namedrops`. Le gel
  total monte de 1,4 s par passe, dans l'écart entre deux passes du même bras
  (jusqu'à 1,7 s).

**Ce qu'on en retient.** NVENC en entrée D3D12, la chaîne que le réglage D3D12
donne à une carte NVIDIA, prend le défaut de NVENC en D3D11
(`nameLinkDropsByDefault`, qui lit l'encodeur de la route D3D12 dans
`SessionInfo`). AMF en entrée D3D12 reste sans : aucun réglage ne le choisit,
puisqu'une carte AMD passe en D3D12 Video Encode, et seule une clé de banc le
fait tourner.

Vérifié le jour même sur l'édition dev qui porte ce défaut
(`0.3.1-8bf1ff36-dev`), par cinq courts streams depuis le Chrome du N95
(`c925d\tools\nd-check3.txt`). La ligne « streaming » porte
`[link drops named]` sur NVENC en entrée D3D12 sans clé, comme sur NVENC en
D3D11. Elle ne la porte pas sur D3D12 Video Encode de la RTX, sur AMF en entrée
D3D12, ni avec `namedrops=0`.

### 8n.30 Pourquoi oneVPL se bloque sous les réparations (§9-25, 29/09/2026)

**La question.** Au §8n.27, avec `namedrops=1`, les trois passes de l'Arc en
oneVPL ont fini en stream mort (« still executing »). La clé `lose=` ne le
reproduisait pas hors ligne.

**Montage.**
- `--native-bench` sur l'Arc de DualRTX (pilote 32.0.101.8993), la page de
  défilement sur son écran, 1080p60 à 20 Mbit/s, chaîne D3D11 (oneVPL), en
  utilisateur.
- Une réparation : l'image perdue est refusée, et l'image suivante ne prédit
  que d'une référence longue d'avant la perte (`mfxExtAVCRefListCtrl`).
- La clé `lose=` gagne une variante `k` : chaque rafale se termine par une
  demande d'image clé, comme quand le lien se vide.
- Un blocage : l'encodage ne se termine pas en 10 s, et la session s'arrête
  sur « still executing ».
- Sorties et scripts : `bench-out\d3d12v2\vplhang`.

**Reproduit hors ligne.** `lose=60x4`, une rafale de 4 pertes toutes les 60
images, bloque le HEVC en 20 à 30 s, 6 fois sur 6. Le §8n.27 n'avait pas
essayé ce rythme.

| Variante (HEVC sauf mention) | Blocages | Réparations réussies |
|---|---|---|
| `lose=60x4`, intra-refresh (le témoin) | 3/3 | 27 à 35 |
| le même sans gouverneur (`governor=0`) | 3/3 | 30 à 58 |
| sans intra-refresh (`intra=0`) | 0/3 | 85 à 91 |
| chaque rafale close par une image clé (`lose=60x4k`) | 0/3 | 56 à 62 |
| vagues d'intra-refresh bout à bout (`irdist=-1`) | **2/2, dès la première réparation** | 0 |
| vagues à 3000 images d'écart, aucune pendant l'essai | 0/2 | 86 et 90 |
| pertes espacées aux départs de vague (`lose=480x4`, `lose=480`) | 0/4 | 2 à 8 |
| H.264, vagues bout à bout ou espacement du moteur | 0/4 | 81 à 87 |
| AV1, vagues bout à bout | 0/2 | 78 et 81 |

- **Il faut une vague d'intra-refresh en cours.** À 60 i/s, une vague dure 120
  images, et deux vagues partent à 480 images d'écart. Les blocages du témoin
  tombent tous dans une vague : dernières réparations réussies aux images 483
  à 600, puis 967. Vagues bout à bout, la première réparation suffit.
- **Les changements de débit n'y sont pour rien** : sans gouverneur, 3 sur 3
  aussi, sans un seul `EncodeReset`.
- **Une réparation isolée passe le plus souvent.** Les pertes espacées aux
  départs de vague n'ont rien bloqué : le blocage est fréquent, pas certain.
- **Le GPU ne se plante pas** : aucun TDR, aucun rapport du noyau. Le runtime
  attend une tâche qui ne se termine jamais.
- **H.264 et AV1 font les mêmes réparations sans broncher.** C'est le HEVC de
  l'encodeur Intel qui ne supporte pas une référence longue imposée pendant une
  vague.

**En production.** La v0.3.1 encode l'Arc et les iGPU Intel en HEVC par
oneVPL, avec l'intra-refresh dès que le client fait le pari du ride-out. Elle
répare chaque perte que ce client signale, par le même chemin. Le 29/09, Bruno a
streamé ainsi la prod de DualRTX depuis son téléphone. Aucun blocage n'est
apparu en 30 jours de journaux, mais le risque existe. Sur `main`, Intel encode
le HEVC en D3D12 Video Encode, qui n'a pas d'intra-refresh : oneVPL n'y garde le
HEVC que si le réglage force D3D11.

**Le correctif (option A, choisie par Bruno le 29/09).**
- En HEVC sous intra-refresh, oneVPL ne marque plus de référence longue
  (`longTermRepairsSafe`, `VplSession.h`). `/start` répond donc
  `ref_invalidation:false`, et chaque perte signalée coûte une image clé.
- H.264, AV1 et le HEVC sans intra-refresh gardent leurs réparations.
- Rejoué sur le même montage, avec le binaire du correctif (sorties et
  script : `bench-out\d3d12v2\vplfix`) :

| Variante | Blocages | Réparations | Images clés |
|---|---|---|---|
| HEVC, `lose=60x4` (le témoin) | 0/3 | 0 | 96 à 106 |
| HEVC, vagues bout à bout, sans gouverneur | 0/3 | 0 | 92 à 106 |
| HEVC sans intra-refresh | 0/1 | 104 | 1 |
| H.264, intra-refresh | 0/1 | 100 | 1 |

- Chaque perte du banc devient une image clé : 4 pertes d'affilée, jusqu'à 4
  images clés. Un vrai client en demande moins : tant que l'image clé demandée
  n'est pas arrivée, il ne redemande qu'au bout d'une seconde.
- Sur la page qui défile à 20 Mbit/s, une image clé pèse 39 à 45 Ko, contre
  31 à 34 Ko pour un delta. Le tampon d'une image la plafonne : elle coûte en
  netteté, pas en débit. Sur 30 s, les octets totaux ne bougent pas (47 à
  55 Mo, contre 52 sans intra-refresh).

## 8o. Linux : la chaîne Vulkan (28/09/2026 →)

Phase 13 du plan D3D12 : la même forme de chaîne sous Linux, en Vulkan Video
là où le pilote l'offre, VA-API sinon. Avant d'en écrire une ligne dans le
moteur, le labo `mw-vk-lab` (`tools/vk-lab`, jamais installé) demande au
matériel ce qu'il sait faire.

### 8o.0 Le 780M de l'UM790Pro sous Mesa 25.2.8 (28/09/2026)

**Montage.**
- UM790Pro, Ubuntu 22.04.5, noyau 6.8.0-138, Radeon 780M (Phoenix,
  VCN 4.0.2). Micrologiciel VCN du paquet `linux-firmware` d'Ubuntu :
  `0x0711300d`, soit ENC 1.19.
- RADV de Mesa 25.2.8, ajouté au préfixe de labo (`~/mesa-25/prefix`,
  `-Dvulkan-drivers=amd`). Le Mesa 23.2 du système n'a pas d'encodage
  Vulkan. Chargeur 1.4.313 (SDK LunarG), en-têtes du module 1.4.364.
- `VK_DRIVER_FILES=<préfixe>/share/vulkan/icd.d/radeon_icd.x86_64.json` et
  `RADV_PERFTEST=video_encode` : Mesa n'expose l'encodeur de VCN 4 d'office
  qu'à partir d'ENC 1.22.
- Binaires de `c60f9816` (`caps`) et `5f1b1300` (`encode`). Sorties dans
  `bench-out\vk-lab`.

**`caps`.**
- Encodeurs : H.264 (High, Constrained Baseline) et HEVC (Main, Main 10).
  Pas d'AV1 : RADV le réserve à ENC ≥ 1.20.
- HEVC :
  - CTB de 64 seulement, transformées de 4 à 32 ;
  - une seule référence active (L0 = 1 en P), 17 emplacements de DPB, tous
    dans une même image (pas de `separate-reference-images`) ;
  - débit : QP constant (`DISABLED`), CBR, VBR ; QP de 0 à 51 ; deux niveaux
    de qualité ;
  - granularité d'accès et d'entrée : 64 × 16 ;
  - retour de l'encodeur : décalage et octets écrits, pas les retouches ;
  - ni intra-refresh ni cartes de QP (les extensions manquent) ;
  - niveau maximal annoncé : 1.0, un champ que RADV ne remplit pas.
- L'entrée de l'encodeur (NV12, et P010 en Main 10) se crée en `STORAGE` ou
  en `COLOR_ATTACHMENT` par plan : la conversion peut écrire directement
  dedans, sans copie.
- Priorités : les quatre familles de files annoncent LOW à REALTIME. En
  utilisateur, HIGH et REALTIME sont refusées (`VK_ERROR_NOT_PERMITTED`)
  partout, file d'encodage comprise ; en root (`CAP_SYS_NICE`), tout est
  accordé.
- Horodatages : 64 bits sur les files graphique et compute, **aucun sur la
  file d'encodage**. Le temps d'encodage se mesure donc côté CPU, ou par les
  horloges calibrées (device, monotonic, monotonic raw).
- Import : les quatre formats de capture (XR24, XB24, XR30, XB30)
  s'importent avec leurs six modificateurs, dont le DCC affichable en trois
  plans. Le plan primaire affiché ce jour-là (1920 × 1080 XR24,
  `GFX11, 64K_R_X, DCC, DCC_RETILE…`) s'importe tel quel ; le curseur
  (256 × 256 AR24 linéaire) aussi.
- Sémaphores : `sync_file` en import et en export.

**`encode`, HEVC 1080p60 à QP 30.**
- Latence : 2,3 ms en moyenne de la soumission au flux en main, 3,0 ms au
  p99, envoi de l'image compris (0,24 ms sur la file compute). Chiffre à
  reprendre avec un micrologiciel qui code juste (ci-dessous) ; le tout
  intra, lui juste, donne 2,26 ms.
- Relus dans chaque en-tête de tranche : le QP est celui demandé (180 sur
  180), la RPS aussi (une référence, utilisée).
- Nos en-têtes : RADV les retouche, et le dit (`hasOverrides`) : tranches
  dépendantes activées, `cu_qp_delta` et tailles de blocs imposés. Il garde
  l'AMP de notre SPS.
- **Au pixel, le flux est faux dès la première image P** : 5 dB, erreurs
  CABAC dans ffmpeg. Avec ou sans SAO, en CBR comme en QP constant, DPB de 2
  ou 3 emplacements, envoi par la file compute ou graphique.
- **En tout intra (`--idr-every 1`), il est juste** : 0 erreur, 27,2 dB
  constants. La même machine en VA-API (`hevc_vaapi`) : 0 erreur.
- Attribué ce jour-là au micrologiciel (Mesa : « VCN 4 FW 1.22 has all the
  necessary pieces to pass CTS » ; en dessous, RADV cache son encodeur
  derrière `RADV_PERFTEST`). **À tort** : la faute était dans notre SPS,
  la profondeur de transformée (§8o.3).

**Ce qu'on en retient.**
- Le produit ne pose jamais `RADV_PERFTEST` : la chaîne Vulkan n'est offerte
  que là où le pilote expose l'encodeur de lui-même.
- La preuve au pixel attrape ce que les en-têtes relus ne voient pas : QP et
  RPS justes, flux faux. Elle a aussi montré plus tard que la cause
  supposée, le micrologiciel, n'était pas la bonne (§8o.3).

### 8o.1 Où la conversion attend derrière un jeu (28/09/2026)

La question qui a tranché G1 sous Windows : une priorité de file fait-elle
passer la conversion devant un jeu qui tient le GPU ?

**Montage.**
- Même 780M. Files Vulkan : RADV 25.2.8 du préfixe. Témoin GL : le Mesa 23.2
  du système, le chemin du produit aujourd'hui.
- La conversion : un Lanczos-2 d'une image 2560 × 1440 vers la luminance
  1920 × 1080 et sa chrominance 960 × 540, 60 fois par seconde. En compute
  (`shaders/convert.comp`), ou en deux passes GLES faisant le même calcul,
  suivies d'un `glFinish` comme dans `GlConvert`. Elle coûte 2,2 ms de GPU
  aux horloges du repos, 0,76 ms quand le GPU est lancé.
- La charge : `mw-gpu-load` au niveau 248, soit 45 i/s et 21,8 ms de GPU par
  image, un jeu qui sature l'iGPU. 60 s au plus ; la garde thermique l'a
  arrêtée trois fois à 85 °C, toujours après les passes retenues.
- Root (`CAP_SYS_NICE`) pour HIGH et REALTIME. Une priorité après l'autre,
  6 s chacune ; l'heure de chaque passe est recoupée avec le journal de la
  charge.
- Binaires de `21fa9c63`. Sorties dans `bench-out\vk-lab`.

| chemin de la conversion | repos | sous la charge : moy. / p99 |
|---|---|---|
| GLES, file graphique, sans priorité (le produit aujourd'hui) | 3,9 ms | 46,0 / 47,5 ms |
| GLES, contexte EGL HIGH | 3,9 ms | 23,3 / 24,3 ms |
| Vulkan, file graphique MEDIUM | 2,7 ms | 45,7 / 47,4 ms |
| Vulkan, file graphique HIGH / REALTIME | 2,7 ms | 23,1 / 23,0 ms (p99 23,9 / 24,0) |
| Vulkan, file compute LOW / MEDIUM | 2,6 ms | 10,5 / 10,6 ms (p99 18,2 / 18,4) |
| Vulkan, file compute HIGH / REALTIME | 2,6 ms | 8,0 / 8,1 ms (p99 14,5 / 15,3) |

- **File graphique** : la conversion attend l'image du jeu en cours. À la
  priorité du jeu, elle attend aussi la suivante : deux images de 22 ms.
  HIGH passe devant l'image suivante, pas au milieu de celle en cours. GL et
  Vulkan font exactement pareil.
- **File compute** : la conversion tourne à côté du jeu, sur les unités qu'il
  laisse, au lieu d'attendre son tour. Sans aucun privilège, 10,6 ms au lieu
  de 46. HIGH retire encore 2,6 ms en moyenne et 4 ms au p99.
- LOW affame la file (plus de 2 s d'attente) : jamais pour le produit.
- Mesa 23.2 répond « HIGH accordé » pour un contexte EGL que le noyau a
  refusé faute de `CAP_SYS_NICE` (`amdgpu_cs_ctx_create2 failed (-13)`) : ne
  pas se fier à la relecture.
- Le jeu passe de 45-46 à 44 i/s pendant les passes : la conversion lui
  coûte 2 à 4 % à 60 conversions par seconde.
- Réserve : l'unique dessin plein écran de `mw-gpu-load` exagère les attentes
  de la file graphique (la leçon de G1). Un vrai jeu (Counter-Strike 2, en
  G5) dira combien il en reste. La file compute ne dépend pas de ce
  découpage.

**Ce qu'on en retient.**
- Sur AMD, le gain de la Phase 13 tient d'abord à la **file compute**, pas à
  l'encodeur : une conversion Vulkan en compute, devant l'encodeur VA-API
  d'aujourd'hui (une route scindée, par DMA-BUF), retire 35 ms sous une
  charge qui sature le GPU. Elle ne dépend ni du micrologiciel VCN ni de
  Vulkan Video.
- Le témoin bon marché : un contexte EGL HIGH dans `GlConvert`, avec
  `CAP_SYS_NICE`, divise l'attente par deux (46 → 23 ms) pour un seul
  attribut.

### 8o.2 L'image capturée dans Vulkan (28/09/2026)

- `mw-vk-lab import` (`83b4cf15`), en root, sur le bureau GNOME de
  l'UM790Pro. Plan primaire 1920 × 1080 XR24,
  `GFX11, 64K_R_X, DCC, DCC_RETILE…` : trois plans dans un seul objet
  (décalages 0, 8 847 360 et 8 896 512). Vulkan attend bien trois plans pour
  ce modificateur.
- Import (image, mémoire, liaison) : 0,02 à 0,04 ms. Copie linéaire : 0,2 à
  0,4 ms de GPU, après la barrière implicite du tampon (`sync_file`, par
  `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`).
- La lecture par Vulkan et celle par EGL (comme `GlConvert`) sont identiques
  au pixel, et l'image est bien le bureau.
- Pareil avec le RADV du Mesa 23.2 du système : l'import n'a pas besoin du
  préfixe, seul l'encodeur en dépend.

**Ce qu'on en retient.** Le premier maillon de la route scindée tient : un
tampon KMS s'importe dans Vulkan sans copie, sur le Mesa d'Ubuntu 22.04.

### 8o.3 Le micrologiciel VCN 1.24, et la vraie cause des P fausses (28/09/2026)

**Montage.**
- Décision §9-16 du plan : le `vcn_4_0_2.bin` de `linux-firmware` en amont
  (11/09/2026, `0x09118022`, soit ENC 1.24, DEC 9, révision 34) posé dans
  `/lib/firmware/updates/amdgpu/`. Le paquet d'Ubuntu n'est pas touché (son
  `vcn_4_0_2.bin` est un lien vers `vcn_4_0_0.bin`, ENC 1.19) ; `amdgpu`
  n'est pas dans l'initramfs, rien à régénérer. Retour arrière : effacer le
  fichier.
- Redémarrage sous Ubuntu garanti par `efibootmgr -n 0001` (`BootNext`, une
  seule fois) : l'ordre permanent du dual boot reste Windows d'abord, GRUB
  démarre son entrée 0 (Ubuntu). Revenu en 45 s, `Found VCN firmware
  Version ENC: 1.24 DEC: 9`, session GNOME et prod relancées.
- RADV de Mesa 25.2.8 (préfixe de labo) et de Mesa 26.2.3 (préfixe
  `~/mesa-26`, RADV seul, libdrm 2.4.134, compilé ce jour) : l'encodeur
  H.264, HEVC **et AV1** est exposé sans `RADV_PERFTEST`.

**Les P restaient fausses.**
- ENC 1.24, sur les deux Mesa : même défaut qu'en 1.19, et même flux à
  l'octet près (IDR 136 932 octets, P 36 639 en moyenne).
- Témoin : ffmpeg 7.1 `hevc_vulkan`, compilé à part. Il ne démarre pas :
  `VK_ERROR_DEVICE_LOST` dès la réinitialisation de la session, sans remise à
  zéro du GPU dans le journal du noyau.
- `--still` (chaque P identique à sa référence) : juste, 0 erreur, le PSNR de
  l'IDR. Dès que l'image bouge, le décodeur décroche après quelques rangées
  de CTB.
- La cause est dans radeonsi, qui pilote le même bloc VCN et code juste en
  VA-API. Il écrit toujours `max_transform_hierarchy_depth_inter/intra =
  log2_diff_max_min_luma_coding_block_size + 1`, soit 4 en CTB 64 : le
  micrologiciel découpe les transformées jusque-là.
- RADV reprend la profondeur de l'application sans la corriger ni la
  transmettre (le labo mettait 2). Les `split_transform_flag` que le
  micrologiciel écrit au-delà ne sont pas lus par le décodeur : un grand
  bloc inter les déclenche, les petits blocs intra de la mire non.

**Avec la profondeur complète** (`--depth`, 4 en CTB 64, désormais le défaut
du labo), HEVC 1080p60, RADV 26.2.3 :

| variante | erreurs ffmpeg | PSNR luma, min / médiane |
|---|---|---|
| QP 30, profondeur 2 (avant) | 32 | 4,9 / 5,4 dB |
| QP 30, profondeur 4 | 0 | 27,2 / 27,5 dB |
| QP de 22 à 42, un pas par image | 0 | 26,7 / 27,4 dB |
| CBR 20 Mbit/s | 0 | 26,6 / 27,5 dB |
| deux références gardées | 0 | 27,2 / 27,5 dB |
| AMP et lissage intra | 0 | 27,2 / 27,5 dB |

- RADV 25.2.8 donne la même chose : profondeur 2 fausse, profondeur 4 juste
  (27,2 / 27,5 dB en QP 30, 26,7 / 27,5 en CBR).
- Latence : 2,4 ms en moyenne de la soumission au flux en main, 3,0 ms au
  p99, envoi de l'image compris.
- **File d'encodage en HIGH** : le périphérique se crée (§8o.0), mais le
  noyau refuse la première soumission (`CS rejected (-22)`, puis
  `VK_ERROR_DEVICE_LOST`), sur les deux Mesa. La conversion en compute HIGH,
  elle, tourne (§8o.1).

**Ce qu'on en retient.**
- Le micrologiciel n'était pas en cause pour les P (§8o.0 corrigé). La mise
  à jour reste utile : l'encodeur est exposé d'office, et l'AV1 arrive.
- Ni le numéro du micrologiciel ni la version de Mesa ne disaient « fiable » :
  ENC 1.24 avec Mesa 26.2.3 codait faux avec notre SPS. Pour le produit, il
  faut une preuve au pixel à l'ouverture de l'encodeur Vulkan (une courte
  séquence connue, encodée puis décodée sur le même GPU, et comparée) et le
  repli automatique sur VA-API (plan, C13.5 et §9-19).
- L'encodeur Vulkan du produit écrira la profondeur complète. Une priorité de
  file d'encodage ne compte que si une soumission passe : on redescend sinon.
- Revu le 28/09 au soir, le micrologiciel 1.19 d'origine remis en place pour
  le H.264 du §8o.7 : RADV 26.2.3 passe aussi la preuve au pixel, en
  profondeur complète, quand `RADV_PERFTEST=video_encode` force l'encodeur.
  Le seuil de 1.22 est un choix de Mesa (la suite de conformance), pas une
  limite du bloc. Le produit ne force rien : sans l'encodeur exposé, il prend
  VA-API.

### 8o.4 La surface de l'encodeur VA-API, écrite en compute (28/09/2026)

Le dernier maillon de la route scindée : une file compute Vulkan qui écrit
directement dans la surface que VA-API encode.

**Montage.**
- `mw-vk-lab vatarget`. La surface est créée comme `VaapiEncoder` crée son
  entrée (`vaCreateSurfaces`, NV12, rien d'autre) et exportée en deux
  couches, une par plan.
- Les plans sont importés comme images R8 et RG8 à ce modificateur, puis
  écrits par un shader compute (`imageStore`, un motif exact sur 8 bits).
- Ils sont ensuite rendus à la famille « foreign », la CPU attend la file
  (l'équivalent du `glFinish` de `GlConvert`), et VA-API relit la surface
  (`vaGetImage`) : chaque échantillon est comparé.
- Mesa 23.2 du système, RADV et VA-API : ce que le produit rencontre sur
  Ubuntu 22.04.

**Résultats.**
- La surface d'entrée est **linéaire** (modificateur 0), pas de 2048, la
  chrominance à 2 228 224 octets dans le même objet.
- RADV y accepte R8 et RG8 en `storage`, en échantillonnage filtré et en copie.
- Import des deux plans : 0,05 ms. Écriture et attente : 0,23 ms en 1080p,
  0,14 ms en 720p.
- VA-API relit exactement ce que Vulkan a écrit : **0 échantillon faux** sur
  2 073 600 de luminance et 518 400 de chrominance, en 1080p comme en 720p.
- Piège de labo : RADV 26 du préfixe, chargé dans le même processus que le
  VA-API du système, hérite du `libdrm_amdgpu` que libva a chargé avant lui
  (même soname) et ne s'initialise pas. Le produit charge les pilotes du
  système, jamais un mélange.

**Ce qu'on en retient.** La conversion Vulkan écrit directement dans la
surface de l'encodeur : ni copie, ni changement de propriétaire de la
surface. La route scindée a ses deux bouts, l'import de la capture (§8o.2) et
cette écriture.

### 8o.5 La route scindée dans le produit, contre GL (28/09/2026)

C13.4 ter : la route scindée (conversion Vulkan en compute, encodeur VA-API)
telle que le produit la construit, contre la conversion GL d'aujourd'hui, au
repos et sous un jeu qui sature le GPU.

**Montage.**
- UM790Pro, 780M, Mesa 23.2 du système (RADV et radeonsi) : ce que le
  binaire du produit charge, puisqu'il porte des capacités de fichier.
- L'édition dev (binaires de `ea8edb89`) en `--native-bench` : capture
  KMS 1920 × 1080 à 60 Hz, HEVC VA-API à 20 Mbit/s, 20 s par passe.
- Quatre conversions :
  - GL sans priorité (le produit avant §9-18, sans `CAP_SYS_NICE`) ;
  - GL en HIGH (§9-18) ;
  - Vulkan compute sans priorité (`convert=vulkan,priovk=normal`) ;
  - Vulkan compute en HIGH (`convert=vulkan`, le défaut de la route quand
    `CAP_SYS_NICE` est tenu).
- Au repos, Chrome fait défiler la page de banc. Sous la charge,
  `mw-gpu-load` au niveau 248 (45 i/s, 21,8 ms de GPU par image) est seul à
  l'écran : la capture voit les images du « jeu ».
- Deux tours au repos, trois sous la charge, dans l'ordre inverse d'un tour
  à l'autre. La garde thermique (85 °C) a coupé la charge au bout de 2 à 4 s
  dans trois passes du deuxième tour, avant la fenêtre du banc : passes
  écartées, refaites au troisième tour après un refroidissement à 47 °C.
- Script et sorties : `bench-out\vk-lab\split-*` et
  `splitbench-2026-09-28.tgz`.

| conversion | repos : moy. / p99 | charge : moy. / p99 | images captées sous la charge | présentation → encodé sous la charge : moy. / p99 |
|---|---|---|---|---|
| GL, sans priorité | 0,86 / 1,34 ms | 33,7 / 40,6 ms | 16 i/s | 38,0 / 44,8 ms |
| GL, HIGH | 0,86 / 1,23 ms | 15,6 / 39,6 ms | 22 à 32 i/s | 19,9 / 44,2 ms |
| Vulkan compute, sans priorité | 0,54 / 0,79 ms | 3,7 / 13,4 ms | 44 i/s | 8,0 / 17,6 ms |
| Vulkan compute, HIGH | 0,57 / 0,93 ms | 4,1 / 14,0 ms | 44 i/s | 8,4 / 18,2 ms |

- **Sous la charge**, la conversion GL attend le jeu : 16 images captées par
  seconde sur 45, et 38 ms de la présentation à l'image encodée. HIGH la
  double sans la sauver (p99 inchangé, 40 ms).
- La route scindée **capte toutes les images du jeu** (44 sur 45 i/s), en
  8 ms en moyenne et 18 ms au p99. HIGH n'y ajoute rien sur cette charge :
  c'est la file compute qui compte, pas le privilège.
- **Au repos**, la route scindée gagne 0,3 ms de conversion (0,55 contre
  0,86) et 0,2 ms de bout en bout.
- L'encodeur VA-API ne bouge pas (4,2 à 4,5 ms) et le jeu garde ses
  44,3 à 45,1 i/s dans les quatre cas : la conversion ne lui coûte rien de
  mesurable.
- Au repos, la page défilée par Chrome ne présente que 27 à 40 i/s, dans
  toutes les variantes : c'est le contenu, pas la chaîne.

**Ce qu'on en retient.**
- Sur le 780M, la route scindée est meilleure partout : un peu au repos, du
  tout au tout sous un jeu. Elle tourne avec les pilotes d'Ubuntu 22.04,
  sans Vulkan Video, et sans `CAP_SYS_NICE`.
- Proposé à Bruno (plan §9-20) : la ligne AMD de la table des vendeurs passe
  à la route scindée (`autoLinuxConversion`). GL reste le repli automatique
  (Vulkan absent, import refusé, périphérique perdu), testé.
- **Décision de Bruno du 28/09 : par défaut.** Sur la capture KMS ; le portail
  garde GL jusqu'à son propre banc. `vaapi` dans l'admin reprend GL devant
  VA-API (design §32.8).
- Leçon de banc : sous `mw-gpu-load` 248, refroidir à 47 °C et attendre 20 s
  avant chaque passe. À 58 °C, le radiateur encore chaud laisse la garde
  couper en quelques secondes.

### 8o.6 La chaîne Vulkan Video et sa preuve au pixel (28/09/2026)

C13.5 : la chaîne entière en Vulkan (`5549c48e`, design §32.7). La preuve au
pixel d'abord, puis la chaîne contre VA-API, au repos et sous la même charge
qu'au §8o.5.

**La preuve au pixel sur le 780M.**
- Le Mesa 23.2 d'Ubuntu 22.04 (celui que charge le produit) ne montre pas
  d'encodeur Vulkan : refus nommé, sans ouvrir de périphérique, VA-API encode.
- RADV 26.2.3 (préfixe, en root) : la preuve passe en 120 à 130 ms à 1080p
  (70 ms à 720p). Pire image 39,8 dB, pire rangée de CTB 37,4 dB, chrominance
  42,1 dB, sur 10 images relues (12 encodées, deux perdues en route).
- Le témoin, profondeur de transformée 2 (la faute du §8o.3) : **5,2 dB**, pire
  rangée 4,7 dB. La preuve échoue, la session prend VA-API et le dit.
- En session (`test_linux_session`), les quatre cas se tiennent, et les flux se
  décodent sans une erreur :
  - la chaîne prise quand la preuve passe ;
  - le témoin refusé ;
  - la chaîne lâchée en plein stream, VA-API sur l'image même ;
  - une chaîne qui ne démarre pas, VA-API dès l'ouverture.

**La chaîne contre VA-API.**
- Même montage qu'au §8o.5, mais en root avec RADV 26.2.3 (les capacités de
  fichier du binaire lui cacheraient `VK_DRIVER_FILES`). libdrm du même
  préfixe pour tout le processus (le piège du §8o.4). VA-API et GL restent
  ceux du système.
- Trois routes : GL → VA-API (HIGH, root tient `CAP_SYS_NICE`), la route
  scindée (Vulkan compute HIGH → VA-API), la chaîne Vulkan Video (Vulkan
  compute HIGH → Vulkan Video). Deux tours au repos, deux sous la charge.
- Sorties : `bench-out\vk-lab\vkvbench-2026-09-28.tgz`.

| route | repos : encodage | repos : présentation → encodé, moy. / p99 | charge : conversion, moy. / p99 | charge : encodage | charge : présentation → encodé, moy. / p99 | images captées sous la charge |
|---|---|---|---|---|---|---|
| GL → VA-API | 3,8 ms | 4,7 / 5,4 ms | 22,4 / 39,6 ms | 4,4 ms | 26,8 / 44,3 ms | 23 i/s |
| Vulkan compute → VA-API | 3,9 ms | 4,5 / 5,3 ms | 11,4 / 35,1 ms | 4,2 ms | 15,6 / 39,4 ms | 38 i/s |
| Vulkan compute → Vulkan Video | **1,7 ms** | **2,3 / 3,0 ms** | 9,7 / 40,8 ms | **15,2 ms** | 24,9 / 52,9 ms | 29 i/s |

- **Au repos, la chaîne Vulkan Video divise la latence par deux** : l'encodeur
  Vulkan rend le flux en 1,7 ms là où VA-API en met 3,8 sur le même bloc VCN.
- **Sous la charge, elle perd** : l'encodage passe à 15 ms en moyenne (45 au
  p95), VA-API reste à 4,2. Le jeu garde ses 43 à 45 i/s.
- Même la route scindée est moins bonne qu'au §8o.5 : 11,4 ms de conversion
  sous la charge contre 3,7 avec le RADV 23.2 du système, au même endroit.

**Pourquoi RADV 26 perd sous la charge.**
- Pas root : la route scindée en root avec le RADV 23.2 du système fait
  4,1 ms de conversion, comme en utilisateur au §8o.5.
- Pas un mélange de pilotes : `mw-vk-lab vatarget` (VA-API 23.2 et RADV 26
  dans un même processus) écrit la surface en 1,2 ms sous la charge, comme
  avec RADV 25.
- Pas l'encodeur : `mw-vk-lab encode` seul, sous la charge, rend le flux en
  2,7 ms avec RADV 25 comme avec RADV 26 (2,4 au repos).
- Avec RADV 25.2.8 dans le produit, tout va bien : chaîne Vulkan Video à
  3,0-3,4 ms de conversion, **2,1 ms d'encodage, 5,1-5,5 ms de bout en bout**
  sur deux tours, 44 i/s captées ; route scindée 3,3 ms de conversion.
- **La cause.** RADV 26 envoie toute la mémoire du périphérique avec chaque
  soumission : sa liste de BO est globale, toujours (celle de RADV 25 était
  propre à chaque command buffer, globale seulement sur demande). Or la
  conversion gardait importés les deux ou trois tampons d'affichage entre
  lesquels le compositeur tourne. Le noyau synchronisait alors chaque
  soumission, conversion comme encodage, avec le compositeur en train d'écrire
  l'image suivante du jeu dans l'un d'eux.
- **La preuve.** RADV 25 avec sa liste globale forcée (`RADV_PERFTEST=bolist`)
  reproduit exactement RADV 26 : conversion 9,8 ms, encodage 16,4 ms, 26,2 ms
  de bout en bout pour la chaîne Vulkan Video ; 11,5 ms de conversion pour la
  route scindée.
- **Le correctif.** Un import par conversion, relâché dès la fin de l'attente
  (0,02 à 0,04 ms, §8o.2), au lieu d'un cache par tampon. Avec RADV 26 sous la
  même charge :

| sous la charge | conversion | encodage | présentation → encodé, moy. / p99 | images captées |
|---|---|---|---|---|
| Vulkan Video, RADV 26, avant | 9,7 ms | 15,2 ms | 24,9 / 52,9 ms | 29 i/s |
| Vulkan Video, RADV 26, après | 3,6 ms | 1,95 ms | **5,5 / 14,1 ms** | 44 i/s |
| Vulkan Video, RADV 25 + `bolist`, après | 3,8 ms | 1,96 ms | 5,7 / 14,4 ms | 43 i/s |
| route scindée, RADV 26, avant | 11,4 ms | 4,2 ms | 15,6 / 39,4 ms | 38 i/s |
| route scindée, RADV 26, après | 3,4 ms | 4,2 ms | 7,7 / 15,9 ms | 43 i/s |
| route scindée, RADV 23.2 du système, après | 3,2 ms | 4,3 ms | 7,5 / 12,4 ms | 43 i/s |

- Deux tours sous la charge pour RADV 26, un pour les autres lignes. Au
  repos, après le correctif : 2,3 ms pour la chaîne Vulkan Video, 4,4 pour la
  route scindée. Le jeu garde 44 à 45 i/s partout.
- Sorties : `bench-out\vk-lab\vkv-investigation-2026-09-28.tgz` (et les
  scripts `vkv-*.sh` à côté).

**Ce qu'on en retient.**
- La chaîne Vulkan Video est la meilleure route mesurée sur le 780M, au
  repos (÷2) comme sous un jeu qui sature l'iGPU (5,5 ms contre 7,7 pour la
  route scindée et 27 pour GL), avec RADV 25 et, corrigée, avec RADV 26.
- Ubuntu 22.04 ne l'aura pas : son Mesa 23.2 n'a pas d'encodeur Vulkan, et la
  preuve le dit. Ubuntu 24.04 à jour a Mesa 25.2.8, la version mesurée ici ;
  26.04 a Mesa 26.0.3.
- Tout import d'un tampon qui ne nous appartient pas doit vivre le temps de
  son usage : avec une liste de BO globale, il pèse sur chaque soumission du
  périphérique.

### 8o.7 H.264 en VA-API : Mesa 23.2 et le micrologiciel VCN 1.24 (28/09/2026)

Vu en C13.4 bis : les flux H.264 de la route GL → VA-API avaient 2 à 8
erreurs ffmpeg par passe, en priorité normale comme en HIGH.

**Ce que c'est.**
- Toujours sur des images P, jamais sur les IDR : surtout les grosses passes
  d'affinage de l'écran fixe, parfois une petite (1 144 octets).
- Toujours la **dernière rangée de macroblocs**, aux derniers macroblocs, avec
  5 à 8 octets qui manquent en fin de tranche (`bytestream -5` à `-8`). La
  panne suit la dernière rangée à 1080, 1072 et 720 : ce n'est pas l'arrondi à
  16 de la hauteur.
- Ce n'est pas le VBV : à 100 Mbit/s, l'IDR fait les mêmes 34 Ko et l'erreur
  reste. HEVC est propre dans tous les cas.

**La cause : la combinaison radeonsi 23.2 + micrologiciel VCN ENC 1.24.**

| radeonsi (VA-API) | micrologiciel VCN | erreurs ffmpeg en H.264 |
|---|---|---|
| Mesa 23.2 (Ubuntu 22.04) | ENC 1.24 (posé le 28/09, §8o.3) | 2 à 8 par passe |
| Mesa 23.2 | ENC 1.19 (celui d'Ubuntu 22.04) | **0**, trois tailles et toute la session de test |
| Mesa 25.2.8 (préfixe, celui d'Ubuntu 24.04) | ENC 1.24 | **0** |

- Retour au 1.19 par un redémarrage sous Ubuntu garanti par `BootNext`, le
  fichier 1.24 mis de côté, puis remis en place pour le démarrage suivant
  seulement.

**Ce qu'on en retient.**
- Un Ubuntu 22.04 d'origine (1.19) encode juste en H.264, sans rien faire.
  La panne venait du micrologiciel posé pour le labo Vulkan.
- Un micrologiciel plus récent que ce que le pilote connaît peut casser un
  chemin qui marchait. Le cas reste rare chez un utilisateur (il faut poser le
  micrologiciel à la main), mais c'est un argument de plus pour relire les
  flux : le produit ne le fait aujourd'hui que pour Vulkan Video (§8o.6).
- L'UM790Pro garde le 1.19 jusqu'à son prochain démarrage, puis le 1.24 avec
  Mesa 25.2.8 (passage en Ubuntu 24.04 le 28/09 au soir), une combinaison
  mesurée juste.

### 8o.8 Ubuntu 24.04 : les trois routes en conditions réelles, Mesa 25.2.8 puis 26.2.3 (29/09/2026)

Le §8o.6 refait comme un utilisateur de 24.04 : le Mesa du système, sans
préfixe ni root.

**Montage.**
- UM790Pro en Ubuntu 24.04.5, noyau 6.8.0-142, micrologiciel VCN ENC 1.24.
- Binaires de `46a73728` compilés sur place (gcc 13.3). `--native-bench` de
  l'édition dev en utilisateur, avec les capacités que donne le lanceur du
  paquet (`cap_sys_admin,cap_sys_nice+p`).
- Le Mesa 25.2.8 de `noble-updates`, puis le 26.2.3 du PPA kisak « fresh »
  (radeonsi passe à ACO). Le compositeur tourne encore sur 25.2.8 pendant la
  seconde série.
- Même banc qu'au §8o.6 : capture KMS 1920 × 1080 à 60 Hz, HEVC à
  20 Mbit/s, 20 s par passe. Deux tours au repos (Chrome fait défiler la page
  de banc) et deux sous `mw-gpu-load` 248, ordre inversé d'un tour à l'autre.
- Sorties : `bench-out\vk-lab\noble-2026-09-29`.

| route | Mesa | repos : encodage | repos : prés. → encodé, moy. / p99 | charge : conversion, moy. / p99 | charge : encodage | charge : prés. → encodé, moy. / p99 | captées sous charge |
|---|---|---|---|---|---|---|---|
| GL → VA-API | 25.2.8 | 4,1 ms | 4,86 / 5,45 | 10,1 / 18,4 | 4,4 | 14,5 / 22,9 | 32 i/s |
| GL → VA-API | 26.2.3 | 4,1 | 4,88 / 5,49 | 10,2 / 22,8 | 4,4 | 14,6 / 27,5 | 32 i/s |
| route scindée | 25.2.8 | 4,1 | 4,77 / 5,41 | 3,7 / 7,1 | 4,3 | 8,0 / 11,6 | 42 i/s |
| route scindée | 26.2.3 | 4,1 | 4,81 / 5,43 | 3,6 / 6,9 | 4,3 | 8,0 / 11,5 | 42 i/s |
| Vulkan Video | 25.2.8 | 1,7 | 2,39 / 2,73 | 3,8 / 7,0 | 2,0 | 5,8 / 9,2 | 42 i/s |
| Vulkan Video | 26.2.3 | 1,6 | 2,29 / 2,59 | 3,8 / 7,2 | 1,9 | 5,7 / 9,2 | 42 i/s |

- **Sous un jeu qui sature l'iGPU**, l'ordre du §8o.6 tient hors du labo :
  - la chaîne Vulkan Video rend l'image en 5,7 ms (9,2 au p99) ;
  - la route scindée en 8,0 ms (11,5) ;
  - GL en 14,5 ms (23 à 28), et ne capte que 32 images sur les 43 à 44 du
    jeu. C'est bien mieux qu'avec le Mesa 23.2 de 22.04 (26,8 ms, 23 i/s au
    §8o.6), mais loin derrière les deux routes Vulkan.
- **Au repos**, la chaîne Vulkan Video reste à la moitié des deux autres :
  2,3 ms contre 4,8. Conversion : GL 0,77 ms, Vulkan 0,65.
- **Deux Mesa, un seul résultat** : 25.2.8 et 26.2.3 donnent les mêmes
  chiffres à 0,1 ms près. Le correctif du §8o.6 (un import par conversion)
  tient avec la liste de BO globale de RADV 26.
- **Sans clé**, le produit prend la route scindée (§9-20) : 4,79 / 4,81 ms.
- Le jeu garde 43 à 44 i/s partout. Au repos, la page de banc présente 52 à
  53 i/s pendant Vulkan Video, contre 39 à 46 pendant les routes VA-API, sur
  les deux Mesa et les deux tours : à revoir sur un contenu à cadence fixe.

**La preuve au pixel.**
- La preuve de Vulkan Video à l'ouverture passe en utilisateur, sur les deux
  Mesa, en 125 à 136 ms : pire image 39,8 dB, pire rangée de CTB 37,4,
  chrominance 42,1. Le témoin à profondeur 2 échoue (5,2 à 6,4 dB) et la
  session prend VA-API.
- Les flux du produit, comparés image par image à une page fixe connue
  affichée 1:1 sans pointeur : au pire 32,4 à 36,1 dB (l'IDR, sur un carré de
  bruit), médiane 47 à 58 dB. H.264 à 100 Mbit/s : 51,8 dB au pire.
- L'encodeur VA-API seul, sur huit images qui bougent (ffmpeg) : HEVC 41,7 dB
  au pire, H.264 39,0. radeonsi 26.2.3 encode juste (26.0 à 26.2.0 cassaient
  l'encodage).
- Aucune erreur ffmpeg, sur aucun flux. **Le H.264 tronqué du §8o.7 a
  disparu** avec le Mesa de 24.04, GL comme route scindée, à 20 et 100 Mbit/s.

**Trois pièges, dont deux corrigés.**
- `scripts/bench/hevc-psnr.py` décodait en `gray` : ffmpeg 6.1 étire alors la
  luma d'un flux marqué « plage limitée » (le VUI des flux Linux) et donne un
  faux 24 dB. Il lit désormais le plan Y du `yuv420p` (`7645f2ef`).
- `test_vulkan_convert` convertissait deux fois le tampon d'affichage encore à
  l'écran, par GL puis par Vulkan : quand l'écran bouge, le compositeur le
  réécrit entre les deux (1 passage sur 4 en échec, page qui défile). Une
  seconde conversion GL encadre désormais la comparaison, refaite jusqu'à
  trois fois si l'écran a bougé (`74137b85`) : 4 passages sur 4 verts, suite
  Linux 6008/6008.
- Un profil GNOME ouvert automatiquement a son trousseau verrouillé : toute
  application qui le demande affiche une invite qui assombrit la capture.
  Chrome de banc en `--password-store=basic`.

**Ce qu'on en retient.**
- En conditions réelles, la chaîne Vulkan Video est la meilleure route au
  repos comme sous un jeu. La route scindée, prise par défaut, la suit, et GL
  ferme la marche.
- Mesa 26.2.3 n'apporte ni gain ni perte mesurable ; l'UM790Pro reste dessus.
- Restent pour G5 : Counter-Strike 2 (une connexion Steam de Bruno) et les
  30 min d'endurance, puis la file d'encodage HIGH sous le noyau HWE 7.0.

### 8o.9 G5 : 30 minutes de Vulkan Video sous une charge qui sature le 780M (29/09/2026)

La partie d'endurance de G5, sans attendre Counter-Strike 2 : la chaîne Vulkan
Video pendant 30 min sous `mw-gpu-load` 248, en utilisateur, Mesa 26.2.3.

**Montage.**
- `--native-bench pipeline=vulkan`, HEVC 1080p à 10 Mbit/s, 1 800 s, le flux
  entier gardé pour ffmpeg.
- La charge relancée en boucle : sa garde thermique la coupe à 85 °C. Elle a
  tourné 1 209 s sur les 1 800, par à-coups, le 780M entre 69 et 88 °C : une
  charge au plafond thermique, plus dure qu'un jeu régulier.
- La mémoire du processus relevée chaque minute. Sorties :
  `/tmp/noble/soak2` sur l'UM790Pro.

**Résultats.**
- 73 204 images, **toutes par Vulkan Video** : aucun repli, une seule image
  clé (la première).
- ffmpeg décode les 73 204 images **sans une erreur**.
- Hôte : 7,8 ms en moyenne, 62,8 ms au p99. Le p99 est celui des reprises de la
  charge : le GPU passe de froid à saturé plusieurs fois par minute. Le premier
  et le dernier sixième ne diffèrent que de 0,8 ms en moyenne.
- Mémoire : 106,2 → 112,9 Mo. Ce sont les lignes du banc, gardées en mémoire
  jusqu'à la fin (~92 octets par image). La même passe au repos (5 520 images)
  reste plate.

**Ce qu'on en retient.** La chaîne Vulkan Video tient 30 min sous une charge
au plafond thermique, sans repli, sans image fausse ni fuite. Pour G5, il ne
manque que le vrai jeu (Counter-Strike 2) et la file d'encodage HIGH sous le
noyau 7.0.

### 8o.10 Le noyau 7.0, et la pile AMD de la 22.04 retirée (29/09/2026)

Deux gestes acceptés par Bruno le 29/09 au matin, sur l'UM790Pro.

**La pile AMD de la 22.04 retirée.** Seize paquets de repo.radeon.com
restaient de la 22.04 :
- amdgpu 6.0.2 : sa libdrm, mise de côté le 28/09, avait éteint la TV ;
- AMF 1.4.33 ;
- l'OpenCL de ROCm 6.0.2.

Le produit n'en utilise rien sous Linux : VA-API et Vulkan passent par Mesa.
La simulation n'emportait rien d'autre : ni Mesa, ni GNOME, ni la libdrm du
système, ni linux-firmware. Les paquets ont été purgés avec leurs dépôts,
leurs entrées ld.so et leur ICD OpenCL. Aucun module DKMS n'était installé.

**Le noyau HWE 7.0** (`linux-generic-hwe-24.04`, 7.0.0-34).
- Installé sans erreur. Tous les micrologiciels Phoenix qu'il demande sont
  présents.
- Le 6.8 reste en secours dans le menu GRUB.
- Au redémarrage, forcé sous Ubuntu par BootNext, SSH est revenu en 35 s.
- La session GNOME, la prod, Wolf et Sunshine sont revenus comme avant.
- Les `REG_WAIT timeout` du 6.8 ont disparu de dmesg.
- ⚠️ amdgpu y devient `card0` (c'était `card1`). Les scripts qui lisent la
  température par `card1` sont à corriger.

**Fumée du produit** (8 s par route, HEVC 1080p60 à 20 Mbit/s, écran animé) :

| route | images | erreurs ffmpeg | hôte, moyenne / p99 |
|---|---|---|---|
| GL → VA-API | 479 | 0 | 8,02 / 10,78 ms |
| route scindée (Vulkan compute → VA-API) | 479 | 0 | 8,78 / 10,49 ms |
| Vulkan Video (preuve au pixel : 141 ms, 39,8 dB) | 480 | 0 | 6,48 / 7,87 ms |
| sans clé (la route scindée, par la table) | 482 | 0 | 8,71 / 10,49 ms |

**La file d'encodage en HIGH, toujours refusée.**
- `mw-vk-lab encode --priority high`, en root, Mesa 26.2.3 : la première
  soumission est rejetée (« The CS has been rejected (-22) », puis
  `VK_ERROR_DEVICE_LOST`), sans un mot dans dmesg. Même rejet en REALTIME.
- À la priorité par défaut, la même mesure passe : 2,46 ms de la soumission au
  flux, preuve au pixel à 40,1 dB.
- Ce n'est ni un privilège (root est refusé), ni Mesa (même rejet sous 25.2.8
  et 26.2.3 en 6.8, et sous 26.2.3 en 7.0). Le noyau refuse en silence toute
  priorité au-dessus de NORMAL sur la file d'encodage du VCN 4.0.2.

**Ce qu'on en retient.** Le produit fait déjà le bon choix : l'encodage à la
priorité par défaut, la conversion en HIGH. Pour G5, il ne manque plus que
Counter-Strike 2.

### 8o.11 Le portail en DMA-BUF, et la route scindée dessus (C13.3 bis, 29/09/2026)

**La question.** La capture par le portail est celle d'une AppImage, qui n'a
pas le droit de lire le scanout. Elle gardait GL devant VA-API, même sur AMD :
les tampons de PipeWire n'avaient pas eu leur banc (design §32.8).

**Ce qu'on a trouvé d'abord : le portail ne donnait jamais de DMA-BUF.**
- Un compositeur ne donne un DMA-BUF qu'à un client qui annonce les
  modificateurs qu'il sait importer (la négociation DMA-BUF de PipeWire).
  `PortalCapture` n'en annonçait aucun.
- GNOME 42, puis 46, donnaient donc de la mémoire partagée. La session
  encodait sur le CPU : OpenH264, H.264 seulement.
- Sur cette route, la paire GPU recevait en plus un render node vide.

**Le correctif.**
- `PortalCapture` annonce d'abord, format par format, les modificateurs
  qu'EGL importe sur le GPU de la session (`GlConvert::importableModifiers`).
  Ils sont marqués « à ne pas fixer » : le compositeur choisit celui qu'il sait
  allouer. La mémoire partagée reste proposée derrière, pour un compositeur qui
  n'en prend aucun.
- La paire GPU reçoit le render node de la carte.
- Si GL lui-même refuse un DMA-BUF du portail, la session rouvre le portail en
  mémoire partagée, sur l'accord déjà donné : pas de nouveau dialogue, et le
  stream continue.

**Montage.**
- UM790Pro, GNOME 46 Wayland, Mesa 26.2.3, noyau 7.0.
- L'app sans capacité de fichier, ce qui la fait passer par le portail. Avec
  une capacité, glibc cache l'environnement (`secure_getenv`), et sd-bus ne
  trouve pas le bus de session.
- L'accord donné le 15/09 sous GNOME 42 vaut toujours sous GNOME 46 : aucun
  dialogue, donc pas de clic de consentement à faire.
- `--native-bench` HEVC 1080p60 à 20 Mbit/s, 20 s par passe, deux tours.
  - Au repos : la page qui défile dans Chrome (en Wayland : sous GNOME 46,
    Xwayland exige une autorisation qu'une session SSH n'a pas).
  - Sous charge : `mw-gpu-load` 248 (43 i/s), refroidi à 47 °C avant chaque
    passe. Un premier essai, refroidi à 58 °C seulement, a perdu deux passes
    à la garde thermique de la charge.
- Sorties et scripts : `bench-out\vk-lab\c13-2026-09-29` (`portalbench`).

**La négociation.** GNOME 46 prend l'offre : d'abord `0x0` (linéaire, le
défaut de la liste), puis il fixe `0x200000010401b04`, en un plan.

**Au pixel** (`test_vulkan_convert`, partie portail). GL et Vulkan écrivent la
même chose à une valeur près, en 1:1 comme réduit : 0 échantillon à plus de 1,
moyenne 0,004 en luma et 0,04 en chroma.

**Les temps.**

| route | repos : conversion moy. / p99 | charge : conversion moy. / p99 | images captées sous la charge |
|---|---|---|---|
| GL → VA-API | 0,84 / 1,35 ms | 30,7 / 43,2 ms | 28 i/s |
| Vulkan compute → VA-API | 0,65 / 1,08 ms | **15,8 / 24,5 ms** | **40 i/s** |
| Vulkan compute → Vulkan Video | 0,65 / 1,10 ms | 17,4 / 25,0 ms | 39 i/s |

- Par le portail, la conversion attend d'abord la copie du compositeur dans le
  tampon de la capture (sa barrière implicite). Sous la charge, cela fait 12 ms
  de plus qu'en KMS (§8o.6 : 3,4 ms pour la route scindée).
- La colonne « présentation → encodé » manque exprès : l'horodatage du portail
  n'est pas sur l'horloge du moteur.
- L'encodeur VA-API ne bouge pas (4,0 à 4,6 ms).

**Ce qu'on en retient.** Sur le 780M, par le portail aussi, la route scindée
gagne au repos, divise par deux la conversion sous un jeu, et capte 40 % d'images
en plus. L'exception du portail est levée (« Go » de Bruno pour C13.3 bis, le
29/09) : la ligne AMD de la table vaut pour le scanout comme pour le portail.

### 8o.12 Le portail en mémoire partagée, par la conversion Vulkan (C13.10, 29/09/2026)

**La question.** Un compositeur qui ne donne pas de DMA-BUF donne de la
mémoire partagée, que GL ne sait pas lire. La session encodait alors sur le
CPU (OpenH264, H.264 seulement). La conversion Vulkan peut-elle la lire, et
que gagne-t-on à encoder sur le GPU ?

**Ce que la conversion fait.**
- D'abord, importer la mémoire là où elle est mappée
  (`VK_EXT_external_memory_host`), pour que le GPU la copie sans que le CPU
  touche un pixel.
- **amdgpu le refuse** (`VK_ERROR_INVALID_EXTERNAL_HANDLE`). Le noyau
  n'importe que de la mémoire anonyme, et celle de PipeWire est un memfd.
- Le repli, retenu pour la session : le CPU copie l'image dans un tampon
  visible du GPU (8 Mo en 1080p), puis le GPU la copie dans une image et la
  convertit comme un DMA-BUF. L'encodage reste celui du GPU.

**Montage.**
- UM790Pro, GNOME 46, l'app sans capacité (le portail), `portaldmabuf=0`
  pour que GNOME donne de la mémoire partagée.
- La page qui défile dans un Chrome **maximisé** : en mémoire partagée,
  GNOME 46 n'enregistre aucune image d'une fenêtre en plein écran (0 image en
  10 s, avec l'ancienne paire CPU comme avec la nouvelle ; en DMA-BUF, 414).
- `--native-bench` 1080p60 à 20 Mbit/s, 20 s, deux tours. Le temps CPU du
  processus par le `time` de bash.
- Sorties et scripts : `bench-out\vk-lab\c13-2026-09-29` (`shmbench`).

| route | CPU de l'hôte | conversion moy. / p99 | encodage | images captées |
|---|---|---|---|---|
| CPU → OpenH264 (H.264), jusqu'ici | 71 % d'un cœur | 1,90 / 2,56 ms | 3,9 ms | 40 i/s |
| Vulkan (copie CPU) → VA-API, H.264 | **5 %** | 1,20 / 1,56 ms | 4,1 ms | 37 i/s |
| Vulkan (copie CPU) → VA-API, HEVC | **5 %** | 1,16 / 1,65 ms | 3,9 ms | 37 i/s |

- Le CPU tombe de 71 à 5 % d'un cœur, pour la même latence. Le HEVC devient
  possible.
- 8 % d'images en moins : la conversion et l'encodage partagent le GPU avec
  la copie que fait le compositeur.

**Ce qu'on en retient.**
- Sur AMD, la mémoire partagée passe par la conversion Vulkan puis VA-API ou
  Vulkan Video, là où la table convertit déjà en Vulkan.
- Intel et NVIDIA gardent la paire CPU jusqu'à leur propre banc (`convert=vulkan`
  la mesure).
- GNOME donne un DMA-BUF depuis C13.3 bis : ce chemin sert aux compositeurs
  qui n'en donnent pas.
- Le plein écran en mémoire partagée reste muet sous GNOME 46. C'est une
  raison de plus pour l'offre DMA-BUF, qui le couvre.

### 8o.13 Le bourrage de RADV, retiré de la chaîne Vulkan Video (29/09/2026)

**Ce qu'on a trouvé.** En relisant unité par unité les flux du banc
d'intra-refresh (§8o.14) : en CBR, RADV complète chaque image jusqu'à son
budget avec des unités de bourrage (type 38 en HEVC), placées après les
tranches. Le moteur les envoyait telles quelles.
- La page qui défile, HEVC 1080p60, 16 Mbit/s tenus par le lien : 23 à 37 % des
  octets. Toutes les variantes envoyaient 16,7 Mbit/s, pour 10,7 à 12,8 Mbit/s
  d'images.
- Une image P de 153 octets était suivie de 41,5 Ko de bourrage.
- La page fixe du banc de C13.11 : chaque image renvoyée pesait 122 Ko. Le
  raffinement « coûtait » 40 Ko + 610 Ko en 5 passes, et le lien retenait 8
  images. VA-API raffine la même page pour 10 à 25 Ko.
- Le bourrage vient toujours après les tranches, jamais devant celle d'une
  IDR : pas de refus d'image clé à craindre (celui du Mac avec Sunshine), rien
  que du débit.

**Le correctif** (design §32.23) : l'encodeur retire le bourrage avant que
l'image parte. Mesuré sur le même montage, avec le même build par ailleurs :

| contenu | avant | après |
|---|---|---|
| page qui défile, balayages toutes les 480 images | 34,2 Ko par image | 24,1 à 24,9 Ko |
| page qui défile, perte → image clé | 34,2 Ko par image | 20,8 à 21,7 Ko |
| page fixe avec un carré qui tourne (`still.html?anim=1`, 20 Mbit/s) | ~41 Ko par image (le budget) | **1,6 Ko** (0,79 à 0,81 Mbit/s) |
| image grise inchangée (`test_vulkan_hevc`) | ~41 Ko | **58 octets** |

- 0 unité de bourrage dans les flux, 0 erreur au décodage par ffmpeg.
- L'encodage ne bouge pas (1,62 ms en moyenne) : le retrait se fait sur place,
  dans une mémoire que le CPU lit en cache.
- Aucun des 75 flux Windows gardés dans `bench-out\d3d12v2` n'a de bourrage
  (D3D12 Video Encode sur les trois GPU, oneVPL, NVENC et AMF).
- Sorties : `bench-out\vk-lab\c13-2026-09-29` (`irbench`, `stillir`).

**Ce qu'on en retient.** Les octets des bancs Vulkan Video d'avant ce
correctif (§8o.6 à §8o.12) comptent le bourrage. Leurs temps restent justes :
le bourrage ne coûtait que du débit.

### 8o.14 L'intra-refresh de la chaîne Vulkan Video (C13.9, 29/09/2026)

**La question.** Un client qui traverse les pertes (le *ride-out*) demande à
l'hôte des balayages d'intra-refresh : l'image se répare d'elle-même, sans
attendre d'image clé. La chaîne Vulkan Video n'en faisait pas.
`VK_KHR_video_encode_intra_refresh` le permet-il sur le 780M, et que coûte un
balayage ?

**Ce que le pilote offre** (RADV, Mesa 26.2.3) : des balayages par blocs, par
rangées ou par colonnes, de 256 images au plus, avec une référence pendant le
balayage. La chaîne prend les colonnes (design §32.24).

**Montage.**
- UM790Pro, capture KMS, HEVC 1080p60 par `pipeline=vulkan`, deux tours, le
  bourrage retiré (§8o.13). Chaque flux est relu par ffmpeg.
- La page qui défile (`scroll.html`), 20 Mbit/s demandés, 16 tenus par le lien
  faute de rapport du récepteur, 20 s par passe :
  - `noir` : ni intra-refresh ni perte, la référence ;
  - `ir` : des balayages de 120 images toutes les 480 (le défaut) ;
  - `ir-b2b` : dos à dos (`irdist=-1`) ;
  - `ir-lose` : une perte signalée toutes les 2 s, guérie par un delta ; le
    balayage repart alors de zéro ;
  - `idr-lose` : sans intra-refresh, chaque perte suivie d'une demande d'image
    clé (`lose=120k`), comme quand le lien se vide.
- La page fixe avec un carré qui tourne (`still.html?anim=1`), 20 Mbit/s tenus
  (`governor=0`), 24 s par passe : deux balayages entiers et le début d'un
  troisième. C'est là qu'un balayage coûte : la règle de l'écart de
  `RateControl.h` vient de l'A380, ~30 Ko par image pendant un balayage contre
  3 Ko entre deux.
- Sorties et scripts : `bench-out\vk-lab\c13-2026-09-29` (`irbench`,
  `stillir`).

**La preuve au pixel.** La révision 2 de l'encodeur fait rejouer les verdicts
gardés. Son témoin balaie maintenant par 4 images dos à dos, à travers les
images perdues et l'image clé demandée de la séquence. Elle passe : au pire
39,6 dB par image et 37,4 dB par rangée de CTB (39,8 et 37,4 sans balayage).

**La page qui défile** (les deux tours, après la première seconde ; le QP est
celui des en-têtes de tranche) :

| variante | débit | Ko par image au p99 | QP moyen | images clés après la première |
|---|---|---|---|---|
| `noir` | 11,6-12,1 Mbit/s | 57-58 | 19,1-19,2 | 0 |
| `ir` (120 toutes les 480) | 11,9-12,2 Mbit/s | 57 | 19,4 | 0 |
| `ir-b2b` (dos à dos) | 12,7-12,8 Mbit/s | 57-58 | 20,0 | 0 |
| `ir-lose` (perte → delta) | 12,7-13,0 Mbit/s | 54-57 | 19,8 | 0 |
| `idr-lose` (perte → image clé) | 10,2-10,7 Mbit/s | 55 | 22,9-23,0 | 9 par passe, 44-45 Ko |

- Espacés de quatre périodes, les balayages ne coûtent rien de mesurable sur
  une image qui bouge : le même débit, 0,2 de QP.
- Dos à dos, ou relancés par une perte toutes les 2 s : 5 % d'octets et 0,6 à
  0,8 de QP de plus.
- La réparation par image clé coûte moins d'octets, mais de la qualité : le
  QP monte à 27,3-27,7 sur les 30 images qui suivent chaque image clé, contre
  19 à 20 autrement. L'image s'adoucit une demi-seconde après chaque perte.
- L'encodage ne bouge pas : 1,62 à 1,64 ms partout.
- Une perte pendant un balayage ne bloque rien : le balayage repart de zéro,
  et 0 erreur au décodage. oneVPL, lui, se bloquait en HEVC sous les
  réparations (§8n.30).
- Les deux images de plus de trois budgets par passe (124 à 160 Ko) viennent de
  la page : elles reviennent toutes les 9,53 s dans les huit passes, balayage ou
  pas.

**La page fixe** (les deux tours) :

| variante | débit | Ko par image pendant un balayage / entre deux |
|---|---|---|
| Vulkan Video, sans intra-refresh | 0,79-0,81 Mbit/s | — / 1,6 |
| Vulkan Video, 120 toutes les 480 | 1,51-1,54 Mbit/s | 10,3 / 1,5-1,6 |
| Vulkan Video, dos à dos | 4,87-4,91 Mbit/s | 10,3 en continu |

- Un balayage coûte ~10 Ko par image sur ce texte, trois fois moins que sur
  l'A380. L'écart de quatre périodes en retire les deux tiers : 1,5 Mbit/s au
  lieu de 4,9.
- Mesurée en passant, la paire GL → VA-API (`pipeline=vaapi`) ne se pose pas
  sur cette page. Son CBR remplit le budget : 14,3 à 14,6 Mbit/s sans
  intra-refresh, 20 Mbit/s avec sa vague (§8o.15).

**Ce qu'on en retient.** La chaîne Vulkan Video balaie comme les encodeurs de
Windows. Le prix est celui d'un balayage : quelques Mbit/s sur un écran fixe
s'ils sont dos à dos, d'où l'écart de quatre périodes, qui vaut ici aussi.

### 8o.15 L'écran fixe : la carte de QP écartée (C13.11), et le CBR de VA-API qui ne se pose plus (29/09/2026)

**C13.11, la question.** Une carte de QP (`VK_KHR_video_encode_quantization_map` ;
sur RADV, des deltas seulement, par cases de 64 px en HEVC) affinerait-elle plus
vite l'écran fixe de la chaîne Vulkan Video ?

**Le banc.** `scroll.html?pause=2` : 2 s de défilement, 2 s d'arrêt, en boucle.
HEVC 1080p60, 20 Mbit/s tenus (`governor=0`), 24 s, capture KMS, le bourrage
retiré (§8o.13). Le journal donne chaque rafale de raffinement ; le QP est
celui des en-têtes de tranche.
- **Vulkan Video** : la chaîne est déjà à QP 18 quand la page s'arrête. Ses
  rafales coûtent **0 Ko** sur 5 passes (« 45 Ko + 0 Ko », « 26 Ko + 0 Ko ») :
  il ne reste rien à affiner.
- **La route scindée** (VA-API) affine en 2 à 23 Ko par rafale, et converge
  aussi.
- Le « 610 Ko en 5 passes » mesuré avant le correctif n'était que du
  bourrage (§8o.13).

**Verdict : mesuré, écarté.** Une carte de QP ne peut rien gagner là où
l'encodeur n'a plus rien à affiner.

**Vu en passant : le CBR de VA-API ne se pose plus sur un bureau presque
immobile.** La page fixe avec un carré qui tourne (`still.html?anim=1`), les
mêmes réglages, deux tours :

| route | sans intra-refresh | avec |
|---|---|---|
| Vulkan Video | 0,79-0,81 Mbit/s | 1,51-1,54 Mbit/s (balayages espacés) |
| Vulkan compute → VA-API (**le défaut sur AMD**) | 14,3-14,4 Mbit/s | 19,9-20,0 Mbit/s |
| GL → VA-API (`vaapi` dans l'admin) | 14,3-14,6 Mbit/s | 20,0 Mbit/s |

- VA-API reste bas 4 s (7 à 13 Ko par image), puis prend 32 à 33 Ko par image
  jusqu'au bout, pour un carré de 48 px. Ce sont de vraies données : de
  l'entropie CABAC, ni bourrage ni `cabac_zero_words`.
- Avec sa vague d'intra-refresh, continue, il prend tout le budget.
- Le 22/09, sous Mesa 23.2, la même page retombait sous 1 Ko par image après
  3,5 s (le commentaire du contrôle de débit de `VaapiEncoder.cpp`). Entre les
  deux mesures ont changé Mesa (26.2.3), le micrologiciel VCN (1.24) et le
  noyau (7.0).
- **À trancher par Bruno**, la route par défaut étant touchée : en chercher la
  cause (le contrôle de débit du pilote, ou nos références), essayer un
  plancher de QP pour VA-API, ou passer AMD à la chaîne Vulkan Video, qui
  reste à 0,8 Mbit/s sur la même page.
- Sorties : `bench-out\vk-lab\c13-2026-09-29`. Les bancs d'après le correctif
  du bourrage (§8o.13 à §8o.15 : `irbench`, `stillir`, `stillbench`,
  `stillsplit`) sont dans `c13-after-filler-2026-09-29.tgz`. Ceux d'avant, dans
  `c13-benches-2026-09-29.tgz`.

### 8o.16 G5 : Rise of the Tomb Raider sur un bureau X11 à deux écrans (30/09/2026)

G5 sous un vrai jeu, sur le 780M saturé, et sur le bureau que le pilote NVIDIA
impose : X11, deux écrans sur deux GPU.

**Montage.**
- UM790Pro, Ubuntu 24.04, Mesa 26.2.3, noyau 7.0. GDM passe en X11 à cause du
  pilote NVIDIA 580 de la GTX 1050. Le 780M est le GPU de X :
  - son écran virtuel « HDMI-A-0 » est à 0,0 en 1920×1080 ;
  - le M27Q de la GTX, puits PRIME « HDMI-1-0 », est à 1920,0 ;
  - la racine fait 4480×1440.
- Le banc intégré de Rise of the Tomb Raider (Feral, Vulkan ;
  `steam -applaunch 391220 -nolauncher -benchmark`) : trois scènes en 1080p,
  synchro verticale et triple tampon coupés, le 780M à 99 %.
- La capture tourne pendant tout le banc du jeu (85 s, HEVC 1080p60 à
  20 Mbit/s, `--native-bench`). Les files sont en priorité haute
  (`CAP_SYS_NICE`).
- Deux correctifs rendent ce bureau capturable : le 5 (`6465fbff`, la fenêtre
  de l'écran dans la racine de X) et le 6 (`f758982c`, la relecture sur les
  « damage » de X). Sans eux : 0 image, puis 1 image en 5 s.
- Cinq passes, refroidies à 50 °C entre deux : le jeu seul, Vulkan Video
  (`pipeline=vulkan`), la route scindée (`convert=vulkan`), GL
  (`convert=gl`), puis le jeu seul à nouveau. Ensuite, 30 min de Vulkan
  Video à 10 Mbit/s, le banc du jeu relancé en boucle.
- Script `~/g5/g5-run.sh`, sorties `/tmp/g5r` sur l'UM790Pro.

**Le jeu, en images par seconde (moyenne de chaque scène).**

| passe | Spine of the Mountain | Prophet's Tomb | Geothermal Valley | moyenne | écart |
|---|---|---|---|---|---|
| jeu seul (1) | 64,0 | 45,8 | 40,7 | 50,17 | |
| Vulkan Video | 63,5 | 44,4 | 39,3 | 49,07 | −2,0 % |
| route scindée | 63,1 | 45,0 | 39,9 | 49,33 | −1,4 % |
| GL | 63,1 | 44,9 | 39,8 | 49,27 | −1,6 % |
| jeu seul (2) | 63,8 | 45,6 | 40,4 | 49,93 | |

- L'écart est pris sur la moyenne des deux passes du jeu seul (50,05).
- Le critère (98 % du jeu seul) est tenu, Vulkan Video tout juste.
- Scène par scène, Vulkan Video perd 0,6 à 3,1 % ; la plus lourde,
  Geothermal Valley, perd le plus. La route scindée et GL perdent 1,3 à 1,8 %
  partout.

**La capture sous le jeu (85 s).**

| route | images (captées + renvoyées) | conversion ms, moy. / p95 / p99 | encodage ms, moy. | hôte (CSV) ms, moy. / p50 / p99 |
|---|---|---|---|---|
| Vulkan Video | 2 981 + 79 | 17,2 / 30,7 / 36,9 | 1,9 | 19,2 / 18,7 / 36,2 |
| route scindée | 2 706 + 118 | 16,7 / 30,7 / 41,0 | 4,4 | 21,4 / 20,1 / 44,0 |
| GL | 2 718 + 102 | 17,0 / 32,8 / 41,0 | 4,4 | 21,6 / 20,5 / 42,2 |

- La conversion attend derrière le jeu (§8o.1), même en priorité haute : 17 ms
  en moyenne.
- On capte environ 33 à 35 images par seconde, quand le jeu en fait environ 49.
- ffmpeg décode les trois flux sans une erreur.

**L'endurance (30 min, Vulkan Video, 10 Mbit/s).**
- 74 530 images (58 768 captées, 15 762 renvoyées), toutes par Vulkan Video :
  aucun repli, une seule image clé.
- ffmpeg décode les 74 530 images sans une erreur.
- Le jeu a tourné 24 min, en 12 boucles complètes de son banc. Au 13ᵉ
  lancement, une fenêtre de message de Feral l'a arrêté : c'est l'outillage,
  pas le produit. Les 5 dernières minutes ne montrent qu'un bureau immobile.
- Hôte : 15,3 ms en moyenne sur le premier sixième (sous le jeu), p99
  37,2 ms. Le dernier sixième (bureau immobile) : 2,2 ms.
- Le jeu, sur les 12 boucles : 62,4, 44,2 et 39,3 i/s en moyenne, moins de
  1 i/s d'écart d'une boucle à l'autre. C'est 97,1 % du jeu seul, mais à
  90 °C tout du long, contre 50 °C au départ des passes courtes.
- Mémoire : 141,1 → 147,5 Mo. Ce sont les lignes du banc gardées jusqu'à la
  fin (~88 octets par image), pas une fuite.

**Ce qu'on en retient.**
- G5 passe sous un vrai jeu : moins de 2 % d'images en moins pour le jeu,
  quelle que soit la route.
- La chaîne Vulkan Video tient 30 min sans repli, sans erreur ni fuite.
- Reste la latence sous un GPU saturé : environ 20 ms en moyenne. Et près
  d'une image du jeu sur trois n'est pas captée, parce que la conversion
  passe après le jeu.

### 8o.17 Le paquet de la CI sur les bancs (30/09/2026)

Le paquet DEV `0.3.1.g5d1` (`5d16861a`, correctifs 1 à 6 du design §32.26),
tel que la CI le livre.

**UM790Pro, le `.deb`, sur le bureau X11 du §8o.16.**
- Installé par `dpkg -i` à côté de la prod 0.2.4, en LAN seul :
  `MW_LAN_ONLY=1` est posé dans le gestionnaire systemd de l'utilisateur le
  temps de l'installation, et la DEV que le postinst relance en hérite. La
  prod n'a pas bougé, et aucun port de tunnel n'a été pris.
- Un seul écran proposé, « Display 1 » (l'AMD). La sonde dit pourquoi
  l'écran de la GTX et l'écran virtuel ne le sont pas.
- Le pointeur, de bout en bout : un Chrome client sur DualRTX vise sept
  points de l'image, et `XQueryPointer` lit sur l'hôte où tombe le pointeur
  de X.

| point de l'image | visé | obtenu | écart |
|---|---|---|---|
| centre | 960,540 | 960,540 | 0,0 |
| 10 %, 10 % | 192,108 | 191,108 | −1,0 |
| 90 %, 10 % | 1728,108 | 1727,108 | −1,0 |
| 10 %, 90 % | 192,972 | 191,972 | −1,0 |
| 90 %, 90 % | 1728,972 | 1727,972 | −1,0 |
| 25 %, 75 % | 480,810 | 480,809 | 0,−1 |

- Un pixel du client vaut ici 1,55 pixel de l'hôte : l'écart d'un pixel est
  celui de l'arrondi. Le matin, avant le correctif 2, le même banc tombait
  de 257 à 2 307 pixels à côté. L'écran de l'AMD était alors à 2560,0.
- Au démarrage, l'app et son worker écrivent chacun une ligne de plus :
  « dbus reply error … Unable to open /proc/<pid>/root ». C'est le thème
  GNOME de Qt : il lit les réglages d'apparence par le portail, qui le lui
  refuse pour la même raison qu'il refusait la capture. Sans effet sur le
  stream.
- Pas vu : l'auxiliaire du portail dans le paquet même, sous Wayland (l'écran
  virtuel). L'UM790Pro reste en X11 tant que le pilote NVIDIA y est, et sous
  X11 aucune route du paquet ne passe par le portail.

**PC ARM (Snapdragon 7c), l'installeur ARM64.**
- Installation silencieuse en 12 s. Stream H.264 par l'encodeur Qualcomm
  (Media Foundation) : 24 images par seconde pendant un balayage de la
  souris.
- ⚠️ **Le pointeur reste peint dans l'image.** Le pilote Adreno n'a pas de
  pointeur matériel : la duplication DXGI le peint dans l'image. Depuis
  `a9350aca` (12/09), la session passe alors à Windows.Graphics.Capture, qui
  le laisse au client.
  - Le matin, le service SYSTEM était arrêté : le worker tournait comme
    l'utilisateur, élevé, et la bascule s'est faite en 240 ms.
  - Le soir, l'installeur avait démarré le service (`--worker-service`), et
    le worker tournait en SYSTEM (`765fd962`, 23/09). Le constat est au
    journal (« Desktop Duplication paints the pointer… »), la bascule jamais.
    Déduction : pour un worker SYSTEM, `WgcCapture::available()` répond non.
  - Défaut antérieur à ce chantier, noté au plan §9 pour décision.

**Mac : pas fait.** L'app DEV de `/Applications` appartient à root : sans
`sudo`, on ne peut ni la déplacer ni la renommer. L'ancienne (`0.3.0.g007`) a
été relancée, avec ses deux autorisations intactes.

## 8p. Framerate « Hôte » : l'âge du contenu (29-30/09/2026, provisoire)

Plan `framerate-hote`, design §33. Tout passe par des clés de banc :
`MW_NATIVE_TUNING=cadence=host|host-ceiling|host-guarded` et `MW_VDD_REFRESH`.
Le critère est l'**âge du contenu affiché** chez le client : depuis quand
existe, sur l'hôte, l'image que le client montre à un instant quelconque
(`scripts/bench/content-age`, README).

### 8p.0 Le montage

- Hôte DualRTX, instance `--dev` (18080/18443), relancée à chaque passe avec
  ses clés (`local_matrix.py`) ; écran virtuel **du produit** (tuile « Virtual
  Display »), rendu par l'Arc (D3D12 Video Encode HEVC) ou par la RTX (NVENC
  D3D11, `--vdd-gpu`), à la taille du client.
- Contenu : `scroll.html?band=time&px=600`, à la fréquence de l'écran, sa bande
  calée par CDP sur l'horloge de l'hôte (40 échanges, < 0,3 ms).
- Clients : Chrome sur DualRTX (iGPU AMD, écran à 60 Hz : mise au point
  seulement, son rAF suit l'écran virtuel) ; mw-mac (M1 Pro, Chrome 154, écran à
  120 Hz, tearing, **Wi-Fi**, CDP par tunnel SSH).
- 30 s mesurées par passe, toutes les images lues (`every: 1`). Sorties dans
  `bench-out/content-age/<série>-v<Hz>-<cadence>-r<n>.{json,host.txt}` ;
  `age.py table <série>` résume.

### 8p.1 L'instrument, vérifié

- Estimateur d'horloge du client contre l'horloge exacte de l'hôte (client sur
  la même machine, même compteur) : +0,01 à +0,08 ms.
- 1 800 bandes lues sur 1 800 images décodées, aucune invalide ; lecture par
  `VideoFrame.copyTo`, sans coût mesurable sur le fil principal. La relecture du
  canevas coûtait 13-14 ms par lecture sur l'iGPU AMD : écartée.
- L'âge de la capture tombe à 1-4 ms de l'E2E de l'overlay.

### 8p.2 Client DualRTX (mise au point), Arc, une passe par case

| Écran virtuel | Auto (60 i/s) | host | host-ceiling | host-guarded |
|---|---|---|---|---|
| 60 Hz | 50,5 | 51,2 | 50,0 | 33,0 (page rapide à ce lancement) |
| 240 Hz | 36,3 | 557 | 561 | 81,9 |
| 500 Hz | 31,7 | 697 | 676 | 81,0 |

L'iGPU décode ~120 images/s en 1440p : tout mode hôte au-delà le noie. Signaux
du crédit sur ce client (240 / 500 Hz) : `decodeQueueSize` 82 / 81,
`pending` 51-60 / 42-49, `delay` 89 / 94, `delay30` 68 / 96, `mixed` 73 / 66.

### 8p.3 Client mw-mac, Arc

Soirée du 29/09, deux passes alternées par case, signal du crédit du plan
(`decodeQueueSize`) :

| Écran virtuel | Auto (120 i/s) | host | host-ceiling | host-guarded |
|---|---|---|---|---|
| 120 Hz | 52,1 | 49,0 | 49,0 | 50,0 |
| 240 Hz | 42,5 | 31,1 | 35,6 | 38,4 |
| 500 Hz | 38,6 | 116 | 185 | 53,6 |

Nuit du 29 au 30/09, **quatre** passes par case, signal `delay` :

| Écran virtuel | Auto | host | host-guarded | avant la capture (Auto) |
|---|---|---|---|---|
| 120 Hz | 52,2 | 51,1 | 53,3 | 16,3 |
| 240 Hz | 41,2 | 35,4 | **35,1** | 5,9 |
| 500 Hz | 43,6 | 188 | 51,0 | 2,5 |

### 8p.4 Client mw-mac, RTX (nuit, deux passes par case, `delay`)

| Écran virtuel | Auto | host | host-guarded | avant la capture (Auto) |
|---|---|---|---|---|
| 120 Hz | 36,1 | 42,9 ¹ | 34,7 | 7,6 |
| 240 Hz | 34,9 | 33,8 | **32,2** | 7,8 |
| 500 Hz | 34,8 | 182 | 115 | 3,8 |

¹ Une passe où la page était lente (11,9 ms avant la capture).

### 8p.4 bis Client N95 (Intel UHD, Chrome 154, écran à 60 Hz, Wi-Fi), Arc, nuit du 30/09

Deux passes par case, **une image lue sur dix** (la copie de chaque bande
coûtait au N95 son débit : 53 images dessinées par seconde au lieu de 59, 87 ms
de capture au lieu de 42). Âge affiché médian, ms :

| Écran virtuel | Auto (59 i/s) | host | guarded `delay` | guarded `pending` |
|---|---|---|---|---|
| 60 Hz | 66,4 | 63,5 | 66,6 | — |
| 240 Hz | **53,0** | 1 179 | 169 | 144 |

Le crédit retient 135 à 146 présentations par seconde : l'hôte n'envoie plus
que ~75 images par seconde, ce que le N95 dessine. Mais 150 à 170 ms de capture
restent : la file est hors du décodeur (transport Wi-Fi ou fil principal), là où
aucun des deux signaux ne regarde.

### 8p.4 ter Le client DualRTX sans le poids de la sonde (30/09, une image sur dix)

| iGPU AMD local, écran virtuel à 240 Hz | Âge affiché | Capture | Dessinées/s |
|---|---|---|---|
| Auto (60 i/s) | **31,1** | 20,0 | 60 |
| host | 55,6 | 57,1 | 233 |
| host-guarded `delay` | 47,2 | 37,6 | 219 |
| host-guarded `pending` | 42,2 | 32,4 | 200 |

L'iGPU décode bien 233 images par seconde : la saturation du §8p.2 venait en
partie de la sonde. Mais à ce rythme chaque image passe plus longtemps dans le
décodeur (57 ms de capture contre 20) : la cadence de l'hôte y **coûte** de la
latence, crédit ou pas.

### 8p.4 quater Le signal `e2e` (30/09, ~02:30)

`host-guarded` à 240 Hz, signal `mw_decodequeue=e2e` (excès du retard depuis
la capture sur l'hôte, 10 s), une image lue sur dix :
- N95 : 141 ms affiché (deux passes ; Auto 53,0) — 180 présentations retenues
  par seconde, 44 images dessinées par seconde, moins qu'Auto ;
- iGPU AMD local : 60,2 ms (une passe ; Auto 31,1, `pending` 42,2).

### 8p.4 quinquies Client UM790Pro sous Windows, en **Ethernet** (30/09, ~02:00-03:00)

Radeon 780M, Chrome 154 sur un écran à 120 Hz, câble 1 Gbit/s ; hôte Arc ; une
image lue sur cinq ; deux passes par case. Âge affiché médian, ms :

| Écran virtuel | Auto | host | host-guarded (`delay`) |
|---|---|---|---|
| 60 Hz, tearing | 43,0 | 43,2 | 43,6 |
| **120 Hz, tearing (aujourd'hui)** | **35,6** | — | — |
| 240 Hz, tearing | 26,4 | 23,8 | **21,8** |
| 60 Hz, vsync | 54,6 | 49,6 | — |
| **120 Hz, vsync (aujourd'hui)** | **39,0** | — | — |
| 240 Hz, vsync | 33,4 | 30,7 | — |

Les p99 restent à 31-49 ms (le Wi-Fi du Mac : 90 à 350). Le crédit ne retient
presque rien (3 présentations par seconde) : le 780M suit 240 i/s.

### 8p.4 sexies Trois hôtes, un client en Ethernet (30/09, ~03:00-04:00)

Client UM790Pro sous Windows (780M, 120 Hz, tearing, câble), une image sur
cinq, deux passes par case. L'écran virtuel du produit rendu tour à tour par
chaque GPU de DualRTX (`--vdd-gpu`). Âge affiché médian, ms (avant la capture
entre parenthèses) :

| Hôte | 120 Hz Auto (aujourd'hui) | 240 Hz Auto | 240 Hz host | 240 Hz host-guarded |
|---|---|---|---|---|
| iGPU AMD (AMF, D3D11) | 45,5 (26,2) | 28,6 (8,9) | 27,8 | **25,9** |
| Arc (D3D12 VE) | 35,6 (16,4) | 26,4 (8,4) | 23,8 | **21,8** |
| RTX (NVENC, D3D11) | 23,7 (7,4) | 25,7 (10,3) | 25,1 | **22,9** |

### 8p.4 septies Deux débits (30/09, ~03:45, série interrompue)

Client UM790Pro en Ethernet, Arc, écran virtuel à 240 Hz. Arrêtée en cours par
Claude Code, la machine manquant de mémoire (Chrome de Bruno : 289 processus,
23,5 Go ; 21 sessions Claude Code, 14 Go) : 5 passes sur 8.

| | 40 Mbit/s (automatique à 120 i/s) | 80 Mbit/s (automatique à 240 i/s) |
|---|---|---|
| Auto | 27,0 (2 passes) | 30,0 (1 passe) |
| host-guarded | 23,8 (2 passes) | — |

Doubler le débit coûte ~3 ms à Auto : des images deux fois plus lourdes à
envoyer et à décoder. À refaire en entier.

### 8p.5 Ce que le banc a appris sur lui-même

- La page de contenu a sa propre chaîne jusqu'à l'écran de l'hôte, qui varie
  d'un lancement à l'autre (5 à 22 ms sur le même écran virtuel à 60 Hz), et
  qui dépend du GPU qui la rend (16 ms sur l'Arc, 8 sur la RTX à 120 Hz) :
  répéter et alterner, et lire aussi `since capture`.
- Le Wi-Fi du Mac : ±10 ms d'une série à l'autre, après la capture.
- L'écran virtuel à 500 Hz n'est pas toujours accepté : sur un XML étranger qui
  a accumulé un second mode à 500 Hz, l'écran n'apparaissait plus du tout, à
  aucune fréquence. `local_matrix.py` remet le XML avant chaque passe.
- Une activation qui échoue laisse le nœud de l'écran virtuel activé sans
  écran : seul le démarrage suivant de l'instance l'éteint (défaut du produit,
  §9 du plan).
- L'écran physique de la RTX de DualRTX a quitté Windows pendant une série de
  bascules de l'écran virtuel (29/09, 21:41), sans revenir au rebranchement.

- La sonde elle-même coûte : copier la bande de chaque image est gratuit pour
  le M1 et lourd pour un N95 (voir §8p.4 bis) ; les passes « une sur dix »
  gardent l'âge affiché, échantillonné juste après chaque image lue. Les chiffres
  de l'iGPU AMD de DualRTX (§8p.2, chaque image lue) en sont gonflés d'une part
  à mesurer.

### 8p.6 Ce qui manque avant la porte

Le coût de la sonde sur le Mac ; les deux débits en entier ; RE9 (sans bande :
caméra et clic → drapeau, avec Bruno) ; iPhone / iPad / Android et caméra
(Bruno). Le contenu « texte » ne se distingue pas du défilement pour cet
instrument : la bande change à chaque image, donc toute page change à chaque
image — seule la charge de l'encodeur diffère.

### 8p.7 L'émission calée (`cadence=deadline`), client N95 (30/09, soir)

Plan §13, P3 ; design §33.8. Hôte Arc (écran virtuel du produit), client N95
(Chrome 154, écran à 60 Hz, **Wi-Fi**), une image lue sur dix, **une passe par
case** (Bruno : aller au plus court vers la conclusion). Âge affiché médian
(p99), ms, et images dessinées par seconde :

| Contenu, peinture | Auto, écran virtuel à 240 Hz | `deadline`, 500 Hz | Ratées |
|---|---|---|---|
| Défilement, vsync | 72,7 (201), 49,8/s | 60,0 (174), 35,1/s | 22 % |
| Jeu à 49-53 i/s, vsync | 71,7 (255), 46,2/s | 60,3 (112), 33,6/s | 11 % |
| Jeu à 75-83 i/s, vsync | 68,7 (148), 42,5/s | 59,4 (206), 35,0/s | 15 % |
| Jeu à 49-53 i/s, tearing | 48,5 (102), 50,6/s | 58,8 (103), 51,0/s | — |

Aussi, défilement en vsync : `deadline` à 240 Hz 61,7 (174), 36,8/s, 22 % de
ratées ; Auto à 500 Hz 67,2 (144), 50,5/s.

- **Hôte** : réveil 90 à 100 µs en retard en moyenne (1 ms au pire), 53-54
  rafraîchissements visés par seconde sur 59, 55 à 65 conversions par seconde
  au lieu de 220 à 420.
- **Client** : capture → prête s'étale de 40 à 60 ms (p95 − médiane) ; la
  marge est au plafond (une période) et 11 à 22 % des rafraîchissements sont
  ratés. Le lien : 16 ms de file en moyenne, des gels jusqu'à 615 ms.
- Le gain de médiane (~11 ms) vient de la réserve retirée ; il se paie en
  fluidité, 35 images dessinées par seconde au lieu de 43 à 50 (deux images
  dans un rafraîchissement, aucune dans le suivant).
- En tearing, les deux cadences envoient chaque image aussitôt ; les +10 ms de
  cette passe unique sont dans le bruit du Wi-Fi (§8p.5).
- **Garde-fou** (`26e6134a`), défilement en vsync à 500 Hz : 70,0 (157),
  50,8/s. Le client juge son lien instable (56 ms d'étalement) et l'hôte
  revient à la cadence d'aujourd'hui.
- Pas de client en Ethernet ce soir-là (UM790Pro sous Ubuntu, Mac en visio) :
  le gain là où la visée tient reste à mesurer.

**VRR** (cas 3 de Bruno). Client Chrome sur DualRTX, flux à ~51 i/s en plein
écran et au premier plan, compteur du M27Q lu par Bruno :
- écran de l'iGPU AMD (FreeSync Premium activé, « Variable refresh rate :
  supported 48-120 Hz ») : **60 fixe**, avec puis sans le réglage Windows
  « Taux de rafraîchissement variable » (activé à 18:52 avec l'accord de
  Bruno : `VRROptimizeEnable=1`) ;
- écran de la RTX (G-SYNC Compatible en fenêtré et plein écran) : **144 fixe** ;
- une ligne de déchirure se voit sur la photo : Chrome présente sans attendre,
  mais l'écran garde sa fréquence.
- Réserve sur le montage : l'écran virtuel à 500 Hz devient principal pendant
  le stream et le rAF du client suit 500 Hz. Le contrôle sans stream (la page
  à 49-53 i/s seule, en plein écran sur l'écran AMD) a été mis en place, sa
  lecture n'a pas été rapportée.

### 8p.8 Décision A : clic → drapeau, RE9, la mise en produit (30/09, soir)

Client N95 (Wi-Fi, écran à 60 Hz, tearing), hôte Arc, cadence d'aujourd'hui ;
deux passes alternées par fréquence, 60 clics par passe (`pass.py --clicks`,
`ef5bb517`) :

| Écran virtuel | Âge affiché, ms (2 passes) | Clic → drapeau, médiane (p90), ~117 clics |
|---|---|---|
| 60 Hz (aujourd'hui pour ce client) | 61,2 / 54,4 | 139,3 (189) |
| 240 Hz (A) | 69,6 / 71,0 | 133,8 (174) |

- Le clic → drapeau gagne 5,5 ms en médiane et 15 au p90 : l'image qu'un clic
  fait apparaître attend le rafraîchissement suivant de l'écran virtuel, 8 ms
  en moyenne à 60 Hz, 2 à 240.
- L'âge affiché va dans l'autre sens sur ces passes, après la capture (+10 ms :
  file du lien, décodage). Le débit demandé est le même (20 Mbit/s) ; c'est le
  régulateur de lien qui a réagi au Wi-Fi autrement d'une passe à l'autre. La
  nuit précédente, sur le même client, 240 Hz gagnait 13 ms (§8p.4 bis) : le
  Wi-Fi du N95 ne tranche pas l'âge affiché.
- **Mise en produit** (`03c189ea`), vérifiée sur une vraie session sans clé :
  « virtual display at 240 Hz for a 59 fps stream (faster than the stream) »,
  flux à 59 i/s ; N95 : 53,5 ms d'âge affiché, clic → drapeau 128 ms (40 clics).
- **RE9 non mesuré.** Piloté par script (copie propre ; préférence GPU et
  `config.ini` rendus, empreinte vérifiée) sur l'écran virtuel rendu par la
  RTX : le jeu tourne (scène sous la pluie, RTX à 98 %), mais la capture de
  l'hôte n'en voit que 3 à 7 présentations par seconde et un `ddagrab`
  d'ffmpeg une image en 20 s — avec et sans le réglage VRR de Windows. À
  refaire avec Bruno (H3.2), le jeu lancé à la main.

## 8q. Flux commun des invités : ce qu'un invité coûte au owner (S0, 30/09/2026)

Plan « flux commun des invités », mesure de référence avant tout changement :
aujourd'hui, chaque invité d'un hôte natif a son propre worker, avec sa capture
et son encodeur. Banc `scripts/bench/shared-feed` (README) : un stream du owner,
puis des invités qui rejoignent par la popin de partage comme une personne (lien,
PIN, bouton Rejoindre), fenêtres de 30 s dans l'ordre 0, 1, 3, 0, 3, 1, 0 invités.
Les étapes de l'hôte pour le owner sont lues image par image dans les messages
`stats` qu'il reçoit (un crochet sur la page, rien ne change dans l'app).

### 8q.0 Le montage

- DualRTX : instance `--dev` du build à HEAD, lancée élevée (classe GPU
  REALTIME pour chaque worker, comme le worker SYSTEM du service). Owner en
  2560×1440 HEVC à 60 i/s (réglages de la matrice), invités en 1920×1080 HEVC à
  60 i/s (leur profil fixe), sans intra-refresh pour les invités.
- Trois écrans, trois GPU : l'écran capturé est celui du GPU encodeur ; le Chrome
  du owner décode sur un deuxième GPU, les trois fenêtres d'invités sur le
  troisième. Page `scroll.html` sur l'écran capturé.
- N95 : édition dev `0.3.1-031d0c5e` installée (worker SYSTEM), écran 1920×1080 à
  60 Hz, en **Wi-Fi** ; owner et invités sur DualRTX.
- L'iGPU AMD mesuré est celui de DualRTX (9900X) : l'UM790Pro est sous Ubuntu.

### 8q.1 DualRTX

| Encodeur | Invités | Total hôte p50 / p99, ms | Encodage p50 / p99, ms | i/s envoyées au owner | Sessions d'encodage (moteur) |
|---|---|---|---|---|---|
| RTX 5060 Ti, NVENC (D3D11) | 0 | 3,58 / 4,07 | 3,07 / 3,54 | 60 | 1 (16 %) |
| | 1 | 3,84 / 6,16 | 3,33 / 5,67 | 60 | 2 (25 %) |
| | 3 | **6,78 / 10,69** | 6,27 / 10,14 | 60 | 4 (45 %) |
| Arc A380, D3D12 VE | 0 | 5,12 / 7,68 | 4,61 / 7,16 | 53 | 1 |
| | 1 | 4,86 / 7,81 | 4,61 / 7,32 | 55 | 2 |
| | 3 | 5,12 / **9,24** | 4,61 / 8,63 | 54 | 4 |
| iGPU AMD (9900X), AMF | 0 | 9,22 / 9,72 | 9,22 / 9,34 | 60 | 1 |
| | 1 | 9,22 / 9,73 | 9,08 / 9,17 | 60 | 2 |
| | 3 | **11,26 / 12,13** | 11,26 / 11,65 | 60 | 4 |

Moyenne des fenêtres de même nombre d'invités (3 à 0, 2 à 1 et à 3). Les
fenêtres rejouées concordent à 0,3 ms près.

- **La RTX paie le plus cher** : chaque session NVENC de plus allonge celle du
  owner. À 3 invités, son total hôte double au p50 (+3,2 ms) et gagne 6,6 ms au
  p99 ; le moteur d'encodage passe de 16 à 45 %.
- **L'Arc garde son p50**, son p99 prend 1,6 ms. Il envoie ~54 i/s au owner
  avec ou sans invités : une limite de la route D3D12 en 1440p60 sur un écran à
  120 Hz, pas un effet des invités.
- **L'iGPU AMD** ne bouge pas avec un invité, prend 2 ms (p50) et 2,4 ms (p99)
  avec trois.
- L'acquisition et la file ne bougent pas : tout se joue dans l'encodage.
- Les invités encodent eux-mêmes en 4,1-7,2 ms (RTX, Arc), 6,7-16,4 ms (AMD).
- L'E2E de l'overlay du owner (10-19 ms) varie d'une fenêtre à l'autre sans
  suivre les invités : la mesure qui tranche est celle des étapes de l'hôte.

### 8q.2 N95 (UHD, D3D12 VE, Wi-Fi)

| Invités | Total hôte p50 / p99, ms | Encodage p50 / p99, ms | Acquisition p50 | i/s envoyées au owner | E2E de l'overlay |
|---|---|---|---|---|---|
| 0 | 7,2-7,7 / 23-28 | 5,6-6,1 / 12,3-12,8 | 0,16 ms | 55-57 | 14-38 ms |
| 1 | **22,5 / 76-127** | 7,7-9,2 / 18-19 | **5,6 ms** | **28-34** | **76 ms à 2,4 s** |
| 2 | la page du deuxième invité ne se charge pas en 120 s (deux essais) | | | | |

Un seul invité suffit à mettre le N95 à genoux : la capture elle-même attend
(5,6 ms d'acquisition au lieu de 0,16), le owner tombe à 28-34 i/s, et sur le
Wi-Fi du N95 les deux flux montants font une file de plusieurs centaines de
millisecondes à 2,4 s. Le flux commun ne retire que la part de l'hôte (un
encodage pour tous les invités) : chaque invité reçoit toujours son propre
exemplaire sur le lien montant.

### 8q.3 En cours de route : S5 à S7 (30/09/2026)

Flux commun branché, même banc, sur la RTX. Ce que S9 devait montrer :
2 sessions d'encodage au plus, quel que soit le nombre d'invités ; sur la RTX,
le total hôte du owner à 3 invités revenu vers la ligne « 1 invité » ; sur le
N95, un deuxième et un troisième invité qui rejoignent.

- **S5, le flux commun** (`171c7988`) :
  - à 3 invités, owner à 3,84/6,06 ms (p50/p99) contre 6,78/10,69 ;
  - **2 sessions NVENC au lieu de 4**, moteur d'encodage à 23 % au lieu de 45 ;
  - worker du flux tué : l'invité revient en 1,8 s, sous le même pipe ;
  - interrupteur à 0 : un encodeur par invité, comme avant.
- **S6, l'arbitrage et la bascule H.264** (`13a41c7a`) :
  - 2 invités HEVC, puis un 3ᵉ qui ne décode pas le HEVC : une seule bascule,
    les trois pages en H.264, toujours 2 sessions NVENC, owner à 4,10/6,59 ms ;
  - les mêmes trois invités H.264 sans flux commun : 8,19/11,73 ms, 4 sessions,
    moteur à 49 % ;
  - trois Chrome sur la même Arc débordent leur file de décodage en H.264
    (6 à 10 fois en 12 s avec le flux, 14 à 73 sans) : capacité du client.
- **S7, la hauteur choisie par le owner** (`2346bcea`) :
  - changée en direct, 1080 → 720 → 1440 avec 2 invités : flux reconstruit en
    1280×720 à 4 444 kb/s, puis en 2560×1440 à 17 776 kb/s ;
  - invités revenus en ~1,5 s ; owner à 3,88/6,13 ms sur les trois fenêtres,
    2 sessions NVENC ;
  - un navigateur sans HEVC rejoint directement en H.264 (un seul join),
    l'autre invité suit par un avis `feedcodec`.

### 8q.4 Mesure finale (S9, 01/10/2026)

Le montage de S0 : trois écrans, trois GPU, les mêmes fenêtres (0, 1, 3, 0, 3,
1, 0 invités, 30 s chacune). Deux différences de la machine :
- l'écran de la RTX avait quitté le bureau : un écran virtuel (VDD by MTT,
  rendu par la RTX) l'a remplacé, en 2560×1440 à 120 Hz ;
- l'écran de l'Arc était à 60 Hz (120 en S0).

Build `43896423` : le flux reste sur la route que la machine choisit (voir
plus bas). La mesure RTX a tourné avec `f9b47174`, dont le flux prenait déjà
cette route : NVENC en D3D11, qui a l'intra-refresh.

| Encodeur | Invités | Total hôte du owner, S0 (p50 / p99, ms) | S9 | Sessions d'encodage S0 → S9 | Moteur S0 → S9 | i/s des invités |
|---|---|---|---|---|---|---|
| RTX 5060 Ti, NVENC (D3D11) | 0 | 3,58 / 4,07 | 3,67 / 4,02 | 1 → 1 | 16 → 17 % | |
| | 1 | 3,84 / 6,16 | 3,71 / 5,96 | 2 → 2 | 25 → 27 % | 60 |
| | 3 | 6,78 / 10,69 | **3,84 / 6,19** | **4 → 2** | **45 → 29 %** | 52-54 (décodés sur l'Arc) |
| Arc A380, D3D12 VE | 0 | 5,12 / 7,68 | 4,86 / 8,45 | 1 → 1 | – → 16 % | |
| | 1 | 4,86 / 7,81 | 5,12 / 9,06 | 2 → 2 | – → 26 % | 61 |
| | 3 | 5,12 / 9,24 | 5,12 / 9,65 | 4 → 2 | – → 29 % | 61 |
| iGPU AMD (9900X), AMF | 0 | 9,22 / 9,72 | 10,24 / 10,99 | 1 → 1 | | |
| | 1 | 9,22 / 9,73 | 10,24 / 12,10 | 2 → 2 | | 61 |
| | 3 | **11,26 / 12,13** | **10,24 / 12,33** | 4 → 2 | | 61 |

La mesure AMD s'est arrêtée à sa cinquième fenêtre : la page d'un invité
relancé n'a pas chargé dans les 60 s (banc, pas l'hôte). Ses fenêtres 0, 1,
3 et 0 sont complètes.

**N95** (UHD, D3D12 VE, Wi-Fi ; édition dev `0.3.1-43896423` ; fenêtres 0, 1,
2, 3, 0 comme en S0) :

| Invités | Total hôte du owner, S0 (p50 / p99, ms) | S9 | i/s envoyées au owner, S0 → S9 | E2E de l'overlay, S0 → S9 | i/s des invités |
|---|---|---|---|---|---|
| 0 | 7,68 / 23,46 | 7,42 / 14,62 | 57 → 58 | 38 → 28 ms | |
| 1 | 22,53 / 75,94 | 22,53 / 83,94 | 28 → 39 | 2,4 s → 82 ms | 34 → 35 |
| 2 | page du 2ᵉ invité jamais chargée | **24,58 / 94,46** | → 35 | → 67 ms | 30, 32 |
| 3 | – | page du 3ᵉ invité jamais chargée en 120 s | | | |

- Avec un invité, le N95 fait deux captures et deux encodages avant comme
  après : le flux commun n'y change rien, et le owner reste à genoux. L'E2E
  sans file de ce soir tient au Wi-Fi autant qu'au code.
- Le **deuxième invité rejoint** (impossible en S0), sans que le owner perde
  plus d'1 ms au p50. Le troisième bute encore : l'hôte ne sert plus sa page.
  Ce qui reste par invité (son worker, son WebRTC, sa propre capture audio)
  et les deux captures au même 1080p suffisent à saturer quatre cœurs.
- Piste pour un hôte faible, hors de ce plan (« owner intouché ») : quand
  l'image des invités serait celle du owner (même taille, même codec), les
  invités pourraient suivre son encodage au lieu d'un second.

- **La RTX**, la plus touchée en S0, retrouve à 3 invités la ligne « 1
  invité » : −2,9 ms au p50, −4,5 ms au p99, pire seconde 7,6 ms au lieu de
  13,0. Deux sessions NVENC au lieu de quatre, moteur à 29 % au lieu de 45.
- **L'Arc** ne payait presque rien en S0 et paie la même chose : +0,6 ms de
  p99 avec un invité, +1,2 avec trois (S0 : +0,1 et +1,6). Sa base est plus
  haute ce soir (8,45 ms de p99 au lieu de 7,68) : il envoie 61 i/s au lieu de
  53, son écran étant à 60 Hz.
- **L'iGPU AMD** prenait 2 ms au p50 et 2,4 au p99 à trois invités en S0.
  Avec le flux commun, son p50 ne bouge plus et son p99 prend 1,3 ms, sur une
  base plus haute ce soir (10,24 ms au p50 au lieu de 9,22, à 61 i/s).
- **Les invités de la mesure RTX** décodaient à trois sur l'Arc, qui mène un
  écran à 60 Hz. Chaque page y débordait sa file de décodage 12 à 14 fois par
  fenêtre de 30 s et demandait une image clé : 52-54 i/s. Dans les mesures Arc
  et AMD, les invités décodent sur la RTX : 61 i/s, aucun débordement. S0
  avait déjà deux invités à 49 i/s sur cet Arc : c'est la capacité du client.
- **Le défaut trouvé en route** (`s9-arc-ir.json`) : le flux exigeait
  l'intra-refresh, et sur l'Arc il quittait pour cela le D3D12 VE pour oneVPL
  en D3D11. Les deux routes se gênaient sur le même moteur :
  - avec un seul invité, le flux encodait en 12,3 ms au p50 (35 au p95) ;
  - le p99 du owner passait de 7,8 à 27 ms.
  Corrigé par `43896423` : le flux demande l'intra-refresh sur la route de la
  machine, et s'en passe là où elle ne l'a pas. Les images clés des invités
  sont alors regroupées et rationnées. Le flux de l'Arc encode ensuite en
  4,1-4,6 ms.

### 8q.5 Les cas durs (S9, 01/10/2026)

Même instance, owner sur l'écran virtuel de la RTX (sauf mention), invités sur
DualRTX. Scripts dans `scripts/bench/shared-feed` : `hard_cases.py`,
`throttled_guest.py`, `share_nonreg.py`, `vd_cold.py` (README).

- **Un invité seul qui part et revient** (bouton Quitter, puis Rejoindre) :
  parti en 2,0 s, revenu en 4,2 s, sur le même worker de flux. Ni relance ni
  arrêt : le flux attend 10 s avant de s'arrêter.
- **Changement de mode de l'écran capturé** sous deux invités (écran virtuel,
  2560×1440 → 2224×1440 → 2560×1440) : une reconstruction du flux par
  changement, et les deux invités suivent sa forme (1920×1080 → 1668×1080 →
  1920×1080), sur le même worker.
- **Worker du flux tué** : mort vue, relance 250 ms plus tard sous le même
  pipe, flux prêt et les deux invités de retour 1,3 s après la mort. Leurs
  pages ont manqué au plus une seconde de statistiques.
- **Un invité qui ne décode pas le HEVC arrive** (troisième) : il rejoint
  directement en H.264, le flux passe en H.264 une seule fois, et les deux
  autres reviennent en H.264 après un avis `feedcodec` chacun.
- **Un invité bridé sous le plancher** : flux sur l'Arc, deux invités locaux
  et un sur le N95 en Wi-Fi, dont le lien descendant passe par le shaper
  WinDivert à 3 Mb/s pendant 25 s (la cible du flux est 10 Mb/s, son
  plancher 6).

  | Images peintes par seconde | Invité 2 (local) | Invité 3 (local) | N95 |
  |---|---|---|---|
  | Avant | 61,9 | 61,9 | 61,7 |
  | Bridé | 61,3 | 61,3 | **28,3** (12 lignes de réparation) |
  | Après | 61,8 | 61,7 | 61,3 |

  - Le débit du flux est descendu à 8 000, 6 400, puis 6 000 kb/s en 2 s, et
    il est resté au plancher tout le bridage. Il est remonté 4,5 s après la fin
    du bridage, et était à 10 000 kb/s 9,5 s après.
  - Les invités locaux n'ont écrit aucune ligne de réparation.
  - Seul le N95 a été bridé (le shaper n'a rien retenu d'autre) : 58 paquets
    jetés en queue de file.
- **Non-régression** : un partage depuis Sunshine (`mw-debian`, H.264) et depuis
  Wolf (`wolf2` sur l'UM790Pro, HEVC). Les deux sont inchangés : la page de
  l'invité propose ses trois qualités, l'invité a sa propre session, image en
  8,6 et 11,5 s, et aucune ligne du flux commun au journal.
- **Écran virtuel ouvert à froid par un invité** (`f9b47174`) : allumé en
  1,4 s, image en 5,5 s ; le deuxième invité arrive en 2,4 s sans nouvelle
  opération ; l'écran s'éteint 4 s après leur départ, même quand le owner
  streame une autre appli de l'hôte. Idem avec `MW_SHARED_FEED=0`.
- **Interrupteur à 0** : l'invité a de nouveau son encodeur (S5, S6, et
  l'écran virtuel ci-dessus).

## 8r. Idées Punktfunk, A0 : le labo des pertes et le plafond de SCTP (01/10/2026)

La question qui décide du chapitre FEC (plan `question-c-est-quoi-functional-
possum.md`, A0) : un canal de données **non ordonné et sans retransmission**
échappe-t-il au plafond que SCTP impose sous pertes au canal vidéo
d'aujourd'hui (§8n.22) ? Rien n'est écrit dans le produit avant la réponse,
sauf les clés de banc (`4815aabd`).

### 8r.0 Le montage

- Hôte : DualRTX, édition DEV bâtie de `main` (`build-pf-dev`), écran de
  l'iGPU AMD (« Display 2 ») en kiosque, HEVC 1080p60 par AMF, 20 Mb/s fixes.
- Client : Chrome 154 sur l'UM790Pro (Ubuntu 24.04, session Wayland), piloté
  par DevTools (`scripts/bench/loss/client-chrome.sh`, `flood_run.py`).
- Lien : `tc netem` sur l'UM790Pro (`netem.py`), aux ports média de l'hôte,
  IPv4 et IPv6 ; l'aller-retour ajouté à moitié dans chaque sens ; pertes au
  hasard, ou en rafales (Gilbert-Elliott, rafales de 4 en moyenne). La limite
  de netem est ouverte à 20 000 paquets : sa ligne à retard compte dedans, et à
  14 000 paquets/s 200 jetaient d'eux-mêmes.
- Mesure : un flood de messages de 1 100 octets sur le canal id 3 (`flood=max`,
  `SctpFlood`), compté par le navigateur (`mw_flood`, `FloodCounter.js`) :
  débit livré (médiane des secondes après 6 s), pertes vues, retard ajouté au
  plus court de la session. Le stream vidéo tourne à côté sur une page fixe.

### 8r.1 Le plafond de SCTP sous pertes (A0.3)

Débit livré, Mb/s, par module de congestion d'usrsctp (`sctpcc=`). Les quatre
premières colonnes : canal non ordonné sans retransmission (celui que le FEC
aurait) ; la dernière : le canal du flood avec la fiabilité de la vidéo
(ordonné, 500 ms).

| lien | RFC 2581 | HSTCP | H-TCP | RTCC | comme la vidéo |
|---|---|---|---|---|---|
| 2 ms, 0 % | 121,7 | 128,2 | 7,3 | 125,9 | 124,7 |
| 2 ms, 1 % | 23,2 | 22,7 | 7,0 | 22,2 | 24,6 |
| 30 ms, 0 % | 60,6 | 60,4 | 1,0 | 60,5 | 60,8 |
| 30 ms, 0,3 % | 5,6 | 5,0 | 1,0 | 5,5 | 5,7 |
| **30 ms, 1 %** | **3,4** | **3,3** | **1,0** | **3,5** | **4,2** |
| 30 ms, 2 % | 2,2 | 2,2 | 0,9 | 2,3 | 3,9 |
| 80 ms, 0 % | 17,9 | 17,8 | 0,4 | 17,2 | 17,7 |
| 80 ms, 0,3 % | 2,3 | 2,2 | 0,4 | 2,4 | 3,7 |
| 80 ms, 1 % | 1,1 | 1,1 | 0,4 | 1,4 | 3,2 |
| 80 ms, 2 % | 0,8 | 1,0 | 0,4 | 0,8 | 3,0 |
| 30 ms, 1 % en rafales | 7,2 | 6,0 | 1,0 | 6,1 | 6,9 |
| 30 ms, 2 % en rafales | 4,3 | 3,8 | 0,9 | 4,2 | 4,8 |

- **Sans retransmission, le plafond reste.** La fenêtre de congestion de SCTP
  se réduit à chaque perte, que le message soit renvoyé ou abandonné : 3,4 Mb/s
  à 1 % et 30 ms, là où la formule de Mathis donne ~3,8 Mb/s pour un paquet de
  1 172 octets. Le canal sans retransmission fait même un peu moins bien que
  celui de la vidéo, qui récupère ses pertes.
- HSTCP et RTCC ne changent rien aux petites fenêtres. **H-TCP est cassé dans
  usrsctp** : 7 Mb/s sans aucune perte à 2 ms, 1 Mb/s à 30 ms.
- Les rafales coûtent moins que des pertes isolées au même taux (7,2 contre
  3,4 Mb/s à 1 %) : SCTP réagit à l'événement de perte, pas au nombre de
  paquets perdus.
- Sans pertes, à 80 ms d'aller-retour, le canal tient ~18 Mb/s : le tampon
  d'envoi du produit (244 Kio pour 20 Mb/s, `SendBacklog`) en est la limite
  probable (25 Mb/s au plus par aller-retour).

### 8r.2 Le canal vidéo d'aujourd'hui sous pertes (A0.2)

Le même montage, sans flood : l'écran de l'iGPU affiche la page à bandes
(`scroll.html?band=1`), et l'échantillonneur de `video_run.py` compte les
images abîmées et les gels sur le canevas du stream (§8n.27). Trois réglages
de récupération sur AMF : le défaut (pertes nommées et invalidation),
`namedrops=0`, `dpb=1` (images clés seules). 30 ms d'aller-retour, 25 s par
phase.

| phase | défaut : abîmées · gels (ms) | `namedrops=0` | `dpb=1` | débit · latence (défaut) |
|---|---|---|---|---|
| 0 % | 0 · 2 (250) | 0 · 3 (367) | 0 · 3 (400) | 18,9 Mb/s · 53 ms |
| 0,3 % | 0 · 4 (500) | 32 · 1 (100) | 160 · 0 | 15,3 Mb/s · 111 ms |
| 1 % | 125 · 5 (550) | 94 · 8 (1 034) | 1 · 5 (2 485) | 11,0 Mb/s · 674 ms |
| 1 % en rafales | 4 · 2 (217) | 3 · 5 (534) | 0 · 5 (851) | 9,1 Mb/s · 203 ms |
| 2 % en rafales | 28 · 3 (400) | 37 · 4 (434) | 0 · 7 (2 285) | 8,0 Mb/s · 413 ms |
| 0 % | 0 · 2 (234) | 0 · 1 (117) | 0 · 1 (117) | 7,6 Mb/s · 36 ms |

- À 1 % de pertes, le gouverneur descend à 11 Mb/s, encore au-dessus de ce que
  SCTP laisse passer : la file monte et la latence passe à 0,5-0,7 s.
- L'image clé seule gèle (2,5 s sur 25 s à 1 %) ; l'invalidation abîme
  (94-125 images) mais gèle peu. Les pertes nommées ne servent qu'à un lien qui
  ne se vide plus : elles ne changent rien aux pertes du réseau.
- Le débit reste en bas après les pertes (7,6 Mb/s à la dernière phase) : le
  gouverneur remonte lentement.
- Les gels de la phase sans pertes (2-3 de ~120 ms) sont le bruit du montage.

### 8r.3 La taille d'un shard, le débit de messages, la porte de jet (A0.4)

- **Le plus grand message d'un seul paquet : 1 156 octets.** À 30 Mb/s,
  1 150 et 1 156 octets font 1,07 paquet par message (la vidéo comprise),
  1 160 en fait 2,07. libdatachannel laisse 1 280 − 12 − 48 − 8 − 40 = 1 172
  octets aux chunks SCTP, moins 16 d'en-tête DATA. Avec l'en-tête FEC de 24
  octets, un shard porterait 1 132 octets.
- **Le fil principal de Chrome** (CDP `TaskDuration`, stream fixe à côté) :

  | messages/s | UM790Pro (Linux, Ethernet) | N95 (Windows, Wi-Fi) |
  |---|---|---|
  | 0 | 2,5 % | 7,1 % |
  | 2 000 | 4,1 % | 13,0 % |
  | 5 000 | **5,8 %** | — (le Wi-Fi plafonne à 3 260 : 13,7 %) |
  | 10 000 | 9,0 % | — |

- **La porte de jet** : un flood rythmé à 20 Mb/s sur le canal non ordonné, à
  30 ms. Dans la seconde où 1 % de pertes arrive, la file de libdatachannel est
  pleine et 85-90 % des messages sont retenus (2-4 Mb/s livrés) ; trois
  secondes après la fin des pertes, tout repasse. La porte de jet de la vidéo
  (`SendBacklog`) se déclencherait comme aujourd'hui.

### 8r.4 Le décodeur (A0.5)

`rs-bench.html` : Reed-Solomon GF(2⁸) comme nanors l'écrit (polynôme 285,
Cauchy), k = 170 shards de 1 104 octets, 200 décodages par ligne, chacun
vérifié. Médiane / p99, en ms.

| machine | JS, 1 eff. | JS, 5 | JS, 20 | WASM SIMD, 1 | WASM, 5 | WASM, 20 |
|---|---|---|---|---|---|---|
| DualRTX (Node 24) | 0,18 / 0,50 | 0,86 / 1,64 | 3,45 / 3,66 | 0,02 / 0,33 | 0,05 / 0,08 | 0,19 / 0,32 |
| UM790Pro (Chrome Linux) | 0,3 / 0,6 | 1,4 / 2,4 | 6,1 / 6,7 | 0 / 1,2 | 0,1 / 0,3 | 0,2 / 0,9 |
| N95 (Chrome Windows) | 0,8 / 2,9 | **3,8 / 5,1** | 17,5 / 21 | 0 / 0,5 | **0,2 / 0,4** | 0,7 / 1,1 |

Le WASM SIMD (noyau assemblé à la main, `i8x16.swizzle` sur deux tables de 16)
va 16 à 20 fois plus vite que le JS. Le JS ne tient pas 2 ms p99 sur le N95 ;
le WASM les tient de loin. Les téléphones (iPhone, Android) restent à mesurer
avec la même page.

### 8r.4 bis Le shaper WinDivert étendu, vérifié

`mwshaper.py` sait maintenant `loss`, en rafales, et `delay`. Vérifié contre
le Chrome du N95 en Wi-Fi (flood rythmé à 8 Mb/s, `--remote` borné au N95 et
au routeur) : 2 % demandés → 2,17 % comptés par le navigateur ; 2 % en
rafales de 4 → 1,84 % ; `delay 20 20` appliqué à 68 284 paquets, sans
désordre. Le Wi-Fi de ce montage plafonne vers 28,7 Mb/s.

### 8r.5 La porte A0

| critère | mesuré | |
|---|---|---|
| canal non ordonné ≥ 1,3 × le débit visé à 1 % et 30 ms (≥ 26 Mb/s pour 20) | 3,5 Mb/s au mieux (RTCC) | **non** |
| transport ≤ 10 % du fil principal à 5 000 messages/s, Chrome desktop | 5,8 % | oui |
| décodage ≤ 2 ms p99 sur mobile, 5 effacements | WASM : 0,4 ms sur le N95 (téléphones à mesurer) ; JS : 5,1 ms | oui en WASM |

**Le premier critère tombe, et de loin** : sur un canal de données, ce n'est
pas la retransmission qui plafonne la vidéo sous pertes, c'est le contrôle de
congestion de SCTP. Un FEC réparerait les trous d'un débit que SCTP ne laisse
pas passer.

### 8r.6 La décision (01/10/2026)

Bruno arrête le chapitre FEC à la porte A0 : rien n'entre dans le produit
(A1-A5 abandonnés). Restent les outils de `scripts/bench/loss/` et les clés de
banc (`loss=`, `sctpcc=`, `flood=`), qui serviront au POC Ultra (U1, U3) ; son
canal non ordonné (id 4) aura le même plafond, et garde donc le doublement des
niveaux grossiers. Deux pistes notées, non ouvertes : un contrôle de congestion
du canal vidéo qui ne lise pas une perte au hasard comme une congestion (usrsctp
patché, le gouverneur seul juge du débit), ou la vidéo sur RTP.

## 9. Pour l'A/B

Le banc encode vers un puits ; l'A/B se fait sur un vrai flux. Une session
native lancée avec la variable d'environnement `MW_NATIVE_TUNING` prend les
mêmes clés que le banc :

```
set MW_NATIVE_TUNING=preset=1
MoonlightWeb.exe --dev --log dev-p1.log
```

puis `preset=1,aq=1`, puis rien (P4). Le log dit « MW_NATIVE_TUNING in effect »
et la ligne « NVENC ready » porte `[bench: preset=P1]`. La variable n'est lue que
par le moteur natif, jamais posée par le produit.

**Sur une édition installée**, la variable n'atteint pas le worker. Il est
SYSTEM, et le service lanceur lui construit l'environnement de SYSTEM. On
ajoute donc à la main `"native_tuning": "namedrops=1"` dans le `settings.json`
de l'édition, celui de l'utilisateur. Le serveur le lit à chaque démarrage de
stream et le passe au worker dans sa configuration : le worker ne le lirait pas
lui-même, son AppData étant celui de SYSTEM (`systemprofile`). Le moteur le
prend quand la variable est absente, et le journal du worker dit « settings.json
native_tuning in effect ». Rien à redémarrer : la clé vaut pour le stream
suivant. Le produit ne l'écrit jamais ; la retirer rend la session au moteur.

## 10. Reproduire

```
# lister écrans et GPU
MoonlightWeb.exe --native-bench display=-1
# une passe
MoonlightWeb.exe --native-bench display=1,seconds=10,bitrate=40000,preset=1,out=p1.csv
# l'iGPU AMD par le pont inter-GPU
MoonlightWeb.exe --native-bench display=1,gpu=2,seconds=10,bitrate=40000
```

Clés d'encodeur : `preset=1..7`, `tuning=ull|ll`, `multipass=off|quarter|full`,
`aq=0|1`, `taq=0|1`, `preanalysis=0|1`, `quality=speed|balanced|quality`,
`tu=1..7`, `vbv=<frames>`, `lowlatency=0|1`, `gpu=<id>`, et pour Intel
`lowpower=0|1`, `mbbrc=0|1`, `extbrc=0|1`, `lowdelaybrc=0|1`, `gaming=0|1`,
`winbrc=<frames>`. La chaîne d'image (plan D3D12) a les siennes, listées au
§14 du design : `pipeline=`, `conv12=`, `enc12=`, `rc12=`, `reencode=`,
`refit=`, `prio12=`, `ddasync=`, `gputiming=`, `strict12=`, `pipelined=`,
`keep12=` sous Windows, `pipeline=vaapi|vulkan`, `convert=`, `priovk=` sous
Linux, et `dump=`, `lose=`, `ramp=` pour le banc lui-même. `namedrops=` ne sert
que sur une vraie session (`MW_NATIVE_TUNING`, §9) : c'est le relais qui la lit. Les A/B du plan
passent par `scripts/bench/ab-native-bench.ps1`, en classe REALTIME par un
exécuteur élevé (§8n.2). Le contenu est affaire d'opérateur : ici
un Chrome dédié en kiosque sur l'écran capturé (`--user-data-dir` séparé,
`--kiosk --window-position=<x>,<y>`), relancé avant chaque passe pour le clip.
