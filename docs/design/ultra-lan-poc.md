# « Ultra » en LAN — le POC d'un codec intra sur GPU

> Une question, chiffres à l'appui : un mode réservé au LAN, où chaque image est
> compressée seule sur le GPU de l'hôte, décodée sur le GPU du navigateur et
> présentée à l'arrivée, bat-il nettement le HEVC d'aujourd'hui en latence de
> bout en bout, à qualité acceptable et à débit raisonnable ? Et où se place-t-il
> face à PyroWave, que Steam Remote Play embarque en bêta depuis le 21/09/2026 ?
>
> Un POC : un « non » chiffré est un résultat. Le plan détaillé (phases, commits,
> efforts) vit hors du dépôt ; ce document en garde les hypothèses, le modèle,
> les portes et les résultats.

## 1. Où il se fait, et ce qui protège le produit

- Sur `main`, comme le reste du travail (décision de Bruno du 02/10/2026).
- Les labos vivent hors du produit : `tools/` (côté hôte), `scripts/bench/ultra/`
  (côté navigateur), `docs/`.
- Le code du POC dans le produit reste derrière deux clés cachées :
  `MW_NATIVE_TUNING=ultra=…` à l'hôte, `mw_ultra=1` au client. Ultra n'est jamais un
  défaut. Un refus ou un échec retombe sur le HEVC d'aujourd'hui, avec une ligne de log.
- Hôte natif Windows, route D3D12 seulement. Sunshine, Apollo, Wolf, MultiSeat et
  les hôtes Linux et macOS ne changent pas.
- Un « oui » à la dernière porte ouvre un plan de mise en produit à part. Il
  couvrirait aussi macOS (Metal, d'après le portage Metal de l'amont) et Linux
  (Vulkan, les shaders de l'amont presque tels quels). Un « non » retire le code
  produit du POC par un commit et garde les labos et le rapport.
- Code tiers : PyroWave (MIT) est porté depuis l'amont, avec sa notice, sous
  `third_party/pyrowave`, version épinglée. Les dérivés GPL (Vibepollo, clients
  Moonlight modifiés) ne sont pas lus : ni leur source, ni leurs shaders.
- Corpus : les images de jeux restent hors dépôt. Seuls les outils et les chiffres
  y entrent.

## 2. Les hypothèses du départ, relues contre les mesures

- **Le codec ne fait pas le gros de la latence partout.** Sur NVIDIA, il pèse
  ~3 ms (encodage 1,9, décodage 1,1) sur un clic → drapeau de ~35 ms à 60 Hz
  (banc §8l, §6c). Sur l'Arc, il pèse 5,4 ms en oneVPL ; sur le N95 7 à 11, sur
  l'iGPU AMD 9 à 17. Le reste vient de la cadence de capture et de la
  présentation chez le client.
- **Une image ne se décode qu'à son dernier octet.** Les formats de texture
  perdent sur le fil ce qu'ils gagnent au décodage : BC1 / ETC2 (4 bpp) font
  8,3 ms par image 1080p sur 1 GbE, contre 0,3 ms en HEVC à 20 Mbit/s. Et aucun
  n'existe à la fois sous Windows et sur mobile : Chrome sous Windows n'expose
  que BC. Ils sont mesurés hors ligne, sans encodeur temps réel.
- **Le transport est la première inconnue.** Le média passe par un DataChannel
  WebRTC. usrsctp démarre lentement après chaque pause (burst max 10, bridage par
  la fenêtre), le débit de Chrome en DataChannel n'a pas de mesure publiée, et le
  produit plafonne à 150 Mbit/s. D'où un labo transport avant tout codec.
- **La présentation peut tout reprendre.** WebGPU coûte 4 ms de rendu sous
  Windows et 10 sous macOS, contre 0,2 à 0,6 ms pour Canvas2D et WebGL
  `desynchronized` (03/09). Les présentateurs sont départagés au photon avant
  d'écrire un décodeur.
- **Le candidat principal** est un codec intra par ondelettes, porté de PyroWave
  (MIT) : CDF 9/7 sur 5 niveaux, blocs de 32×32 coefficients décodables seuls,
  sans codage entropique. Moins de 0,1 ms d'encodage et de décodage en 1080p sur
  un GPU récent ; 170 Mbit/s en 1080p60 pour une image jugée sans défaut. Aucun
  portage navigateur n'existe : le décodeur est à écrire, en WGSL ou en shaders
  WebGL2 selon le présentateur qui gagne.

## 3. Le modèle de latence — la boussole

Latence d'une image = attente de la capture + acquisition + conversion +
**encodage** + **sérialisation** (taille ÷ débit du lien) + pile réseau et
navigateur + **décodage** + attente de présentation + dalle. Le codec agit sur
les termes en gras, et sur la présentation si son décodeur impose une API de rendu.

Sérialisation d'une image 1080p au débit ligne, sans en-têtes (borne basse) :

| 1080p | Kio / image | Mbit/s à 60 i/s | 1 GbE | Wi-Fi ~400 Mbit/s réels |
|---|---|---|---|---|
| HEVC 20 Mbit/s | 41 | 20 | 0,33 ms | 0,8 |
| PyroWave, seuil subjectif (1,37 bpp) | 346 | 170 | 2,8 | 7,1 |
| ASTC 6×6 (3,56 bpp) | 900 | 442 | 7,4 | 18,4 |
| BC1 / ETC2 RGB (4 bpp) | 1 013 | 498 | 8,3 | 20,7 |

Les paquets SCTP de 1 280 octets ajoutent ~11 % ; un DataChannel qui ne tient
que 80 % du lien, 25 % de plus : PyroWave au seuil passe à 3,2-3,8 ms. Le labo
transport remplace ces facteurs par la mesure.

Segment codec prédit (encodage + sérialisation + décodage, 1080p60, 1 GbE, client
de bureau) :

| Hôte | HEVC aujourd'hui | Ultra PyroWave 170 Mbit/s |
|---|---|---|
| RTX (NVENC P1) | ≈ 3,4 ms | ≈ 3,6-4,2 |
| Arc (D3D12 VE) | ≈ 3,8 | ≈ 4,1-4,7 |
| N95 (oneVPL) | ≈ 10,5 | ≈ 6,5 |
| iGPU AMD (AMF) | ≈ 10,5-18,5 | ≈ 6,5 |

Lecture honnête : légère perte sur NVIDIA et sur l'Arc, gain de 4 à 12 ms sur
les iGPU. Côté client, un décodeur matériel mobile lent ferait pencher vers
Ultra, un présentateur plus lent que Canvas2D vers HEVC. Hors codec, la cadence
de capture coûte une demi-période (8,3 ms à 60 Hz, 2,1 à 240) : sur NVIDIA, elle
pèse plus que le codec. La barre à battre est donc « HEVC réglé Ultra »
(120 i/s, écran virtuel à 120 ou 240 Hz, P1, tearing, « Auto » avec détection),
pas le HEVC par défaut. Un codec intra paie chaque image : son débit double avec
la cadence, celui du HEVC beaucoup moins.

## 4. Le socle hérité du plan D3D12 (relevé du 02/10/2026)

- **Chaîne par GPU** (§32.19) : Intel en D3D12 (conversion D3D12 sur la file
  DIRECT, D3D12 Video Encode, débit maison, deux images en vol sur un GPU à
  mémoire propre) ; NVIDIA et AMD en D3D11 (NVENC, AMF), D3D12 n'y fait pas mieux
  (G4). Tout refus ou panne revient à D3D11 sans couper le stream.
- **Files** (G1, banc §8n.2) : `CreatorID` propre et priorité `GLOBAL_REALTIME`
  quand le processus tient REALTIME, repli en HIGH. Sur la file COMPUTE, la v1
  était préemptée par le jeu sur l'Arc et affamée sur l'iGPU AMD (117 / 249 ms) :
  un encodeur en shaders partage les unités du jeu, et se mesure sous RE9 et sous
  charge, sur cette politique de files.
- **Temps GPU** : `gputiming=1` encadre chaque soumission d'encodage de deux
  horodatages (`gpu_encode_us`). L'Arc les écrit avant que l'image soit codée et
  ne rapporte rien. La poignée de main DDA reste `ddasync=gpu`.
- **Cadence** : l'« Auto » avec détection (§33.10 de `native-capture-encoder.md`)
  porte déjà le stream au-dessus de la fréquence du client quand l'image y
  rajeunit. Il fait partie de la barre.

## 5. Les phases et les portes

| Phase | Ce qu'elle fait | Porte |
|---|---|---|
| UA | « Auto » avec détection, dans le produit | porte UA (Bruno, 02/10 : par défaut, avec une sûreté) |
| U0 | Budget de latence : E2E par image, âge du contenu, « HEVC réglé Ultra », borne Steam, les deux TV | **U0** |
| U1 | Labo transport : DataChannel à haut débit, réglages usrsctp | **U1** |
| U2 | Labo hôte : corpus, qualité, formats de texture hors ligne, PyroWave de référence, portage HLSL, sous charge | — |
| U3 | Labo navigateurs : planchers natifs, capacités, présentateurs au photon, décodeur, sonde TV | **U2** |
| U4 | Intégration derrière les clés cachées | — |
| U5 | Banc comparatif HEVC / Ultra / Steam PyroWave | **U3** (verdict) |
| U6 | Clôture | — |

Chaque porte donne un rapport chiffré et une recommandation ; Bruno tranche.
Chacune peut arrêter le POC. Les deux portages lourds (encodeur HLSL, décodeur
navigateur) ne commencent qu'après U0 et U1.

- **U0** : on continue sur les couples hôte × client où le gain prédit, sous des
  hypothèses favorables à Ultra, vaut au moins 2 ms ou 20 % de l'E2E instrumentée.
  Aucun couple : le POC s'arrête, et les gains hors codec partent dans la liste
  du plan natif.
- **U1** : sur 1 GbE, Chrome de bureau, 60 i/s, images de 350 Kio, étalement +
  RTT min / 2 au p50 ≤ 1,08 × débit ligne + 1 ms et p99 ≤ + 4 ms ; aucune erreur
  de réception UDP ; transport ≤ 10 % du fil principal du client. Le plafond
  mesuré devient l'enveloppe du codec. Sous 170 Mbit/s en filaire : plan B RTP
  sondé, puis arrêt ou décision de Bruno.
- **U2** : qualité ≥ HEVC P1 à 20 Mbit/s sur le contenu animé, à ≤ 200 Mbit/s ;
  lisibilité du texte fixe jugée par Bruno. Encodage p99 ≤ 1 ms sur la RTX,
  ≤ 2 ms sur l'Arc, ≤ 3 ms sur les iGPU, sous charge. Décodage p50 ≤ 1 ms sur un
  bureau, ≤ 3 ms sur M1 et iPhone ; présentateur sans vsync de plus.
- **U3** : E2E par image, médiane ≤ « HEVC réglé Ultra » − 2 ms (ou − 20 %) avec
  un p99 pas pire ; clic → drapeau pas pire ; qualité acceptée à l'œil par Bruno ;
  30 min sans gel ; 1 % de pertes sans gel de plus d'une image. Issues : « oui »,
  « oui limité » (par exemple hôtes à iGPU et clients filaires), « non ».
- Les portes se jugent en Ethernet 1 GbE ; le Wi-Fi est mesuré et rapporté,
  jamais bloquant.
- **Les TV** (décision du 02/10) : budget en U0, sonde du décodeur Ultra en U3.
  Elles deviennent une cible de U4 seulement si, en 720p, décodage + présentation
  tiennent sous 14 ms par image à plus de 30 images affichées par seconde. Elles
  ne bloquent aucune porte.

## 6. Résultats

*(rempli porte par porte)*

### 6.1 U0.2 — l'E2E par image (03/10/2026, `ea3d313c`)

Le POC a besoin d'une latence mesurée d'un bout à l'autre, image par image, et
non d'une somme d'étapes. La latence de l'overlay additionne des étapes, chacune
chronométrée sur sa propre horloge et moyennée sur sa propre fenêtre : une étape
que personne ne chronomètre n'y figure pas.

- **L'horloge commune existait** (plan 1, cadence de l'hôte) :
  - le pong porte les µs de l'hôte (`DataChannelRelay`, champ `host`) ;
  - `util/ClockEstimator.js` ajuste un décalage et une dérive sur les échanges
    proches du RTT le plus court ;
  - ses tests (dérive, RTT asymétrique, saut d'horloge, valeur aberrante) sont
    dans `ContentAgeProbe.test.js`.
- **Ce que U0.2 ajoute** : `stream/FrameLog.js`. Il met l'horodatage de chaque
  image (`backendTs`) sur l'horloge du client, avec sa propre estimation sur
  60 s, nourrie par le ping de 2 s. Il en tire l'âge de l'image à la fin de son
  dessin. On le retrouve :
  - dans le détail de latence, en ligne « Mesurée (hôte → dessin) » (moyenne et
    p99 sur 2 s). Elle est affichée, pas additionnée : c'est ce que la somme
    devrait lire, et l'écart entre les deux est une étape que personne ne
    chronomètre ;
  - dans la ligne `[perf]` (`e2e measured`) ;
  - dans un journal de toutes les images : un anneau de colonnes de
    16 384 images (une minute à 240 i/s), lu par CDP avec `mwFrameLog.csv()` ou
    `summary()`. `age.py run` le vide au départ et l'enregistre en
    `<tag>.frames.csv` à côté du JSON de la passe.
- **Où le trajet commence** :
  - hôte natif : à la présentation de l'image à l'écran, à la milliseconde. Il
    lit 0 à 2 ms de trop, parce que les deux moitiés de l'horodatage sont
    arrondies à la milliseconde ;
  - hôte GameStream : à l'arrivée de la première image du stream au backend.
    Sa capture, son encodage et son trajet jusqu'au backend manquent, du même
    montant sur chaque image.
- **Limites** :
  - un lien plus lent dans un sens que dans l'autre (la montée en Wi-Fi) fausse
    l'âge de la moitié de l'écart, sur chaque image ;
  - seul le décodage sur le fil principal est couvert, pas le worker (option) ;
  - AV1 n'a pas d'horodatage d'hôte sur ce chemin.
- **Vérifié** :
  - Vitest : 7 tests (âge, horloge pas encore prête, dérive de 50 ppm sur 3 min
    avec un ping toutes les 2 s, bouclage 32 bits, horodatage aberrant,
    anneau et CSV, résumé) ; suite complète 1192/1192.
  - En vrai stream (03/10, 08:37) : deux passes locales sur DualRTX, hôte Arc
    (D3D12 VE), HEVC 2560×1440 à 60 i/s, écran virtuel à 240 Hz, client sur
    l'iGPU AMD. Toutes les images dessinées ont un âge (1200 et 1199) :

    | Passe | Médiane | Moyenne | p99 | Somme des étapes (overlay) |
    |---|---|---|---|---|
    | r0 | 10,5 ms | 13,3 ms | 28,4 ms | 11,3 ms |
    | r1 | 10,2 ms | 12,8 ms | 27,2 ms | 11,7 ms |

    - Les médianes des étapes du journal (présent → arrivée 8,2-9,0 ms,
      décodage 0,6, attente 0,0, dessin 0,2) s'additionnent à 9,0-9,8 ms, sous
      la médiane de bout en bout : c'est cohérent.
    - La somme des étapes de l'overlay lit 1,1 à 2 ms sous la moyenne mesurée.
      Une partie de cet écart est l'arrondi de l'horodatage (0 à 2 ms) ;
      l'autre reste à attribuer.
    - L'horloge : 226-227 échanges (le ping de 2 s et ceux de la sonde d'âge),
      RTT minimal 0,3 ms, dérive estimée 0,4-0,9 ppm (0 en vrai : une seule
      machine), aucun saut. L'estimation sœur de la sonde d'âge, faite sur les
      mêmes pongs, tombe à +0,02 / +0,03 ms de l'horloge vraie.
    - Le contrôle croisé (09:50, deux passes de plus, `ua-u02d-*`) : la bande
      était illisible dans la matinée parce qu'une invite pare-feu de Windows,
      ouverte à 06:38 par le banc T7 pour un exe sans règle, restait posée en
      0,0 de l'écran capturé. Bruno l'a autorisée, et la bande se relit. Image
      par image, le journal et la colonne `capture` de la sonde d'âge
      concordent : −0,09 ms de médiane (p10 −0,19, p90 +0,01) sur 296 et 285
      images appariées. Même horodatage, même horloge : c'est le contrôle
      attendu.
    - **Ce que le contrôle a montré en plus : la sonde d'âge retarde les images
      qu'elle lit.** Elle en lit une sur quatre (`every 4`) ; ces images-là
      attendent 12,2 ms entre leur décodage et leur dessin, les autres 0,0.
      Médianes de bout en bout : 22,0 ms pour les images lues, 8,9 pour les
      autres. La sonde ne calcule ses âges que sur les images lues : ses
      chiffres absolus (`drawn`, `shown`, `capture`) portent ce retard, sur ce
      client du moins (iGPU AMD de DualRTX, Chrome). Les écarts entre modes,
      mesurés avec la même sonde, le portent des deux côtés. Le coût sur les
      autres clients (Mac, N95, UM790Pro) est à mesurer de la même façon,
      avec le journal.
    - Pièges de la matinée : l'instance dev écoute maintenant sur 8080/8443
      (et non plus 18080/18443), et une première passe a été arrêtée par
      Claude Code, faute de mémoire.

### 6.2 U0.2 bis — la sonde d'âge ne retarde plus ce qu'elle mesure (03/10/2026)

Le contrôle de U0.2 (§6.1) a montré que la sonde d'âge du contenu retardait le
dessin des images qu'elle lisait. Elle copiait la bande sur le fil principal,
avant le dessin de l'image. Elle passe maintenant l'image à un worker
(`bandReadWorker.js`, `b83f3dac`), qui fait la copie. Si le navigateur refuse
de transférer l'image, ou si le worker meurt, la sonde copie comme avant. Le
résumé de chaque passe dit par quel chemin la copie est passée (`reader`), et
ce que le fil principal a encore payé par lecture (`readCost`, affiché par
`age.py`). Avec `MW_BENCH_INLINE_READ=1`, une passe copie comme avant : c'est
ce qui a servi à mesurer l'avant et l'après (`659a017b`).

Mesure du 03/10 (12:43-13:00) : hôte Arc, stream 60 i/s, une passe par mode,
deux sur DualRTX. On compte l'attente entre le décodage d'une image lue et le
début de son dessin, donnée par le journal par image ; les autres images
attendent 0,0 à 0,1 ms.

| Client | Avant (fil principal) | Après (worker) | `capture` de la sonde, avant → après |
|---|---|---|---|
| DualRTX, client local (iGPU AMD) | 11,3 / 13,2 ms | 0,1 / 0,1 ms | 19,4 / 25,5 → 8,9 / 9,1 ms |
| N95, Wi-Fi | 7,3 ms | 0,2 ms | 49,8 → 34,8 ms |
| UM790Pro sous Windows, Ethernet | 4,0 ms | 0,1 ms | 16,9 → 12,1 ms |

- **Les âges du contenu mesurés jusqu'ici étaient trop hauts, à peu près de ce
  coût.** La sonde ne calcule ses âges que sur les images qu'elle lit, et
  chacune portait son retard : jusqu'à 12 ms sur le client local, 7 sur le N95,
  4 sur l'UM790Pro. Sur le Mac, ce coût n'a pas été mesuré.
- **Les écarts entre modes** (UA, §8t du banc) portent ce retard des deux
  côtés. Ils restent comparables, tant que le coût ne change pas d'un mode à
  l'autre. Une image lue de plus par seconde, à 240 i/s, aurait pu en ajouter :
  ce n'est pas vérifié pour les passes passées.
- La colonne `capture` de la sonde rejoint maintenant la médiane du journal
  par image (8,9 contre 9,2 ms en local) : la sonde ne mesure plus son propre
  retard.

### 6.3 U0.3 — les séries du 03/10/2026 (N95, UM790Pro, iPhone, Mac)

Hôte DualRTX, client le N95 (Chrome, Wi-Fi), de 15:57 à 16:50 : 20 passes,
chacune avec 30 s d'âge du contenu, le journal par image et 30 clics. Deux modes
alternés D U D U dans chaque case, l'écran virtuel rendu par le GPU de la case :

- **D, « HEVC par défaut »** : le produit d'aujourd'hui, Auto avec détection ;
- **U, « HEVC réglé Ultra »** : la barre (§2), 120 i/s demandés, écran virtuel
  à 240 Hz, tearing, détection ;
- **A** : U en AV1. Les cases AV1 alternent U A U A.

`pass.py --codec` vient de `69475ba6`. Médianes de deux passes par mode (l'âge
montré de A sur l'Arc, d'une seule : l'autre n'a rien pu lire) :

| Hôte | Mode | Âge montré | E2E par image | Clic → drapeau (clics mesurés) | Plus longue coupure du lien |
|---|---|---|---|---|---|
| RTX (NVENC) | D | 44,9 ms | 28,0 ms | 83,0 ms (53 / 60) | 0,8 s |
| RTX (NVENC) | U | 270 ms | 242 ms | 135 ms (22 / 60) | 8,8 s |
| Arc (D3D12 VE) | D | 58,2 ms | 39,9 ms | 88,6 ms (54 / 60) | 1,2 s |
| Arc (D3D12 VE) | U | 130 ms | 114 ms | 204 ms (12 / 60) | 4,4 s |
| iGPU AMD (AMF) | D | 51,7 ms | 31,3 ms | 93,9 ms (46 / 60) | 2,4 s |
| iGPU AMD (AMF) | U | 320 ms | 295 ms | 101 ms (17 / 60) | 14,8 s |
| Arc (oneVPL) | A | 448 ms | non mesuré | aucun (0 / 60) | 31 s |
| RTX (NVENC) | A | 671 ms | non mesuré | 185 ms (8 / 60) | 20,8 s |

- **Sur le N95 en Wi-Fi, demander 120 i/s fait décrocher le lien.** Le lien se
  coupe plusieurs secondes et le débit retombe de 7 à 5 Mbit/s. Le stream ne
  livre que 41 à 82 i/s, et plus d'un clic sur deux n'obtient pas de drapeau. Le même
  écart se répète sur les trois GPU, passe après passe : ce n'est pas un
  incident. Le mode par défaut, lui, reste à la cadence du client : sa détection a
  essayé 116 i/s et y a renoncé (la file du décodeur se remplissait, puis
  l'image arrivait trop tard).
- **La cause : la vidéo attend dans usrsctp, sur l'hôte.** Une série de plus
  (18:00, RTX, D U D U, `relaylog=1`, `scripts/bench/wifi/flagpath.py`) coupe
  le trajet de chaque image. Sur toutes les images de la minute des clics :

  | Mode | Réseau p50 / p90 | dont avant de quitter usrsctp, p50 / p90 | Décodage d'une image de drapeau |
  |---|---|---|---|
  | D | 18-22 / 43-67 ms | 12-13 / 35-54 ms | 2-3 ms |
  | U | 38-49 / 204-263 ms | 25-32 / 186-232 ms | 18-22 ms |

  L'air (la moitié du RTT de SCTP) ne prend que 24-28 ms au p90, et SCTP ne
  retransmet rien. C'est la fenêtre de congestion d'usrsctp qui retient la
  vidéo à 120 i/s, comme dans W1 du plan Wi-Fi. Le décodeur du N95 ralentit
  aussi, mais il ne compte que pour une quinzaine de millisecondes. Pour
  corriger, il faut agir sur l'envoi (le plan Wi-Fi), pas sur le décodeur.
- **L'AV1 est pire encore sur ce client.** Le décodage prend 0,9 à 1,1 s par
  image, et la bande sort gris-violet, illisible sur l'Arc. Le N95 ne décode
  pas l'AV1 1080p à cette cadence. Il n'est pas vérifié si son décodeur AV1
  est matériel ou logiciel.
- **Trou de l'outil, corrigé ensuite.** Le journal par image (U0.2) n'avait
  vu aucune image AV1. La voie AV1 donne au décodeur un horodatage inventé et
  un tampon d'hôte nul, pour que le pacer présente au décodage, et le journal
  lisait ce zéro. Depuis `1cdb4eb9`, le tampon suit l'image jusqu'au journal
  seul. Le pacer, la grille de vsync, la détection et la sonde d'âge ne voient
  toujours rien en AV1 : **la détection de l'« Auto » ne mesure donc pas
  l'AV1**. C'est noté, non corrigé. La correction n'est pas encore vérifiée en
  vrai stream.
- **Pas de case sous charge GPU dans cette série.** `mw-gpu-load` s'arrête au
  bout de 60 s, plus court qu'une passe. Il faudra le brancher dans `pass.py`,
  entre la calibration et la mesure.

**L'UM790Pro en Ethernet** (même jour, 18:33-19:20). Client UM790Pro sous
Windows (Chrome, 780M, écran virtuel à 120 Hz, câble 1 Gbit/s), même hôte et
mêmes cases, `relaylog=1` sur toutes les passes. Le `build\` de 18:02 contient
`e0324f4e` (clé `retrcut` du plan Wi-Fi, éteinte par défaut : rien ne change
sans elle). Médianes de deux passes par mode :

| Hôte | Mode | Cadence | Âge montré | E2E par image, médiane / p99 | Clic → drapeau (clics mesurés) |
|---|---|---|---|---|---|
| RTX | D | 239 i/s | 23,8 ms | 8,4 / 126 ms | 34,8 ms (58 / 60) |
| RTX | U | 120 i/s | 24,7 ms | 9,8 / 24,7 ms | 32,9 ms (57 / 60) |
| Arc | D | 238-239 i/s | 27,1 ms | 10,2 / 25,3 ms | 38,5 ms (58 / 60) |
| Arc | U | 120-121 i/s | 25,0 ms | 12,9 / 30,3 ms | 36,4 ms (56 / 60) |
| iGPU AMD | D | 227-234 i/s | 24,6 ms | 11,4 / 33,2 ms | 42,5 ms (58 / 60) |
| iGPU AMD | U | 119-121 i/s | 28,9 ms | 13,9 / 27,2 ms | 40,0 ms (58 / 60) |
| Arc | A (AV1) | 120-124 i/s | 27,1 ms | 15,5 / 41,0 ms | 44,0 ms (28 / 60) |
| RTX | A (AV1) | 120-121 i/s | 28,3 ms | 11,8 / 30,3 ms | 35,3 ms (58 / 60) |

- **En Ethernet, l'Ultra tient, mais le mode par défaut fait déjà aussi bien.**
  La détection monte à 230-240 i/s et y reste. U, à 120 i/s, fait jeu égal :
  son âge médian par image est de 1 à 3 ms plus haut, mais sa queue est plus
  courte (p99 de 25-30 ms, contre 25-126 ms pour D, le pire étant la RTX à
  240 i/s). Le N95 en Wi-Fi est deux à trois fois plus lent, même en D (âge de
  45-58 ms, clic de 83-94 ms).
- **La fenêtre de congestion d'usrsctp ne retient la vidéo qu'en Wi-Fi.** Ici,
  le réseau a un p90 de 7 à 15 ms, dont 5 à 12 ms avant de quitter usrsctp,
  quel que soit le mode, contre 186-232 ms sur le N95 en U.
- **L'AV1 tient sur le 780M, sans rien gagner** : 1 à 4 ms de plus que le HEVC
  à la même barre, pour 20 à 50 % de débit en plus (36-47 contre 29-32 Mbit/s).
- **Outil.** `1cdb4eb9` est vérifié en vrai stream : les passes AV1 ont leur
  âge par image. Une passe est illisible (Arc, AV1, A1) : tous les pixels lus
  de la bande et du drapeau sont blancs (255), alors que le décodage et l'âge
  par image étaient normaux. La passe suivante sur le même hôte est passée, et
  ses clics manquent au tableau (28 / 60). Cause probable : la fenêtre blanche
  du kiosque de contenu, déjà vue le 22/09 (`acceptance/run.py`,
  `content_start`). Le banc d'âge l'ouvre sans la vérification d'image qui
  relance un kiosque resté blanc (`probe=False`). Le calage de la bande, par
  DevTools, réussit quand même. Ce n'est pas vérifié.

**L'iPhone de Bruno** (même soir, 21:26-21:37, Safari, Wi-Fi). L'hôte est la
`--dev` de DualRTX, avec son écran virtuel au défaut du produit (rendu par
l'Arc, D3D12 VE Intel). Quatre passes en alternance Auto, 120, Auto, 120 (la
page de banc défile, l'iPhone reste immobile 60 s), relevées sur les captures du
détail de la latence. Sur iOS, ni la sonde d'âge ni le clic → drapeau ne
tournent sans console : seule la ligne « Mesurée » de U0.2 donne l'âge.

| Mode | Cadence, taille | « Mesurée » moyenne / p99 | Décodage | Réseau |
|---|---|---|---|---|
| Auto | 60 i/s, 2532×1170 | 41,2 / 69,1 puis 47,9 / 80,4 ms | 5,6 puis 9,7 ms | 4,5 à 40 ms (une coupure de 0,37 s) |
| 120 | 99 i/s, 2336×1080 | 39,8 / 67,2 puis 30,5 / 56,4 ms | 7,3 puis 4,7 ms | 5,3 à 17 ms |

- **Le 120 gagne de 5 à 10 ms sur l'iPhone**, avec deux passes par mode
  seulement, en Wi-Fi.
- **Le décodage HEVC matériel de l'iPhone est mesuré pour la première fois** :
  5 à 10 ms en moyenne, 8 à 21 ms au pire.
- **À creuser** : 38 à 47 % d'images comptées « dropped (jitter) » dans les deux
  modes, alors qu'aucune n'est perdue sur le réseau.
- **Lien** : sur iOS, la `--dev` ne se joint pas à son adresse du LAN
  (`https://192.168.1.66:8443/`). Safari n'étend pas au WebSocket de
  signalisation l'exception de certificat acceptée pour la page, et le journal
  de l'hôte dit « certificate unknown » toutes les 4 s. Son lien de staging
  (`stream.dev.moonlightweb.top/<id>`, vrai certificat, vidéo restée en direct
  sur le LAN) marche.

**Le Mac M1 en Wi-Fi** (même nuit, 22:49-23:19, Chrome de banc, capot fermé).
Même hôte, mêmes cases en HEVC (le M1 ne décode pas l'AV1), `relaylog=1`.
Médianes de deux passes par mode :

| Hôte | Mode | Âge montré | E2E par image, médiane / p99 | Clic → drapeau (clics mesurés) |
|---|---|---|---|---|
| RTX | D | 29,4 ms | 14,1 / 165 ms | 63,7 ms (58 / 60) |
| RTX | U | 36,2 ms | 22,4 / 304 ms | 60,9 ms (55 / 60) |
| Arc | D | 37,1 ms | 22,4 / 157 ms | 72,2 ms (58 / 60) |
| Arc | U | 43,9 ms | 26,4 / 184 ms | 71,0 ms (56 / 60) |
| iGPU AMD | D | 32,7 ms | 17,0 / 145 ms | 74,9 ms (55 / 60) |
| iGPU AMD | U | 43,2 ms | 24,6 / 228 ms | 71,2 ms (57 / 60) |

- **Le défaut monte déjà à 114-125 i/s**, la cadence de l'écran du Mac, et fait
  mieux que U de 7 à 11 ms d'âge. Le clic est le même dans les deux modes.
- **Le Mac tient les 120 i/s, contrairement au N95.** Son p90 avant de quitter
  usrsctp est de 50 à 79 ms dans les deux modes, contre 186-232 ms sur le N95
  en U et 5-12 ms en Ethernet. Le Wi-Fi du Mac fait attendre, sans décrocher.
- **Bilan provisoire de U0.3** : sur ordinateur, la barre « HEVC réglé Ultra »
  ne bat l'« Auto » détecté sur aucun client. Le N95 en Wi-Fi décroche à
  120 i/s, et l'UM790Pro et le Mac font jeu égal ou mieux en Auto. Sur
  l'iPhone, le 120 gagne 5 à 10 ms. Restent l'iPad, RE9 sur la RTX, les cases
  sous charge GPU et Android.

### 6.4 U0.3 — synthèse provisoire (04/10/2026)

**La barre à battre n'est pas « HEVC réglé Ultra », c'est l'« Auto » détecté.**
Sur les trois clients d'ordinateur, la barre du plan (§2 : 120 i/s, écran
virtuel à 240 Hz, tearing, P1) ne fait jamais mieux que l'« Auto » avec
détection (UA, sur `main`). Sur un client qui suit, la détection monte d'elle-même
au-dessus : 230-240 i/s sur l'UM790Pro, la cadence de l'écran (120 i/s) sur le
Mac. Sur un client qui ne suit pas (le N95 en Wi-Fi), elle reste à la cadence
de l'écran, là où forcer 120 i/s fait décrocher le lien.

| Client, lien | « Auto » détecté : âge montré / clic | Barre Ultra : âge montré / clic | File usrsctp p90 (D / U) |
|---|---|---|---|
| UM790Pro, Ethernet | 24-27 / 35-43 ms | 23-29 / 33-40 ms | 5-12 / 5-12 ms |
| Mac M1, Wi-Fi | 29-37 / 61-75 ms | 36-44 / 61-71 ms | 50-67 / 55-79 ms |
| N95, Wi-Fi | 45-58 / 83-94 ms | 130-320 ms / un clic sur deux perdu | 35-54 / 186-232 ms |
| iPhone, Wi-Fi (« Mesurée ») | 41-48 ms | 31-40 ms | — |

**Le budget d'un clic sur l'UM790Pro en Ethernet** (`flagpath.py`, médianes
des 20 passes, en ms) : montée du clic 1,5-3, le drapeau dessiné sur l'hôte
12-16 (la boucle de messages de l'overlay de banc, pas le produit), jusqu'à la
capture 1-5, encodage 2-6, envoi 0,2, réseau 6-10, décodage 0,4-5, dessin
0,2-6. Hors drapeau de banc, il reste 20 à 27 ms. Un codec intra ne peut
gagner que sur l'encodage et le décodage, soit 3 à 10 ms à se partager, et la
cadence (déjà à 240 i/s) n'a plus de marge.

**Ce que cela change pour la suite du POC (proposition pour la porte U0)** :
- La référence de U3 et U5 devient l'« Auto » détecté, plus la barre §2. Le
  critère de §2 (− 2 ms ou − 20 % de médiane, p99 pas pire) se lit contre
  elle.
- Le Wi-Fi n'est pas un terrain pour Ultra. Même en HEVC à 20-30 Mbit/s, la
  fenêtre de congestion d'usrsctp retient déjà la vidéo à 120 i/s (plan Wi-Fi,
  W1). À 150-200 Mbit/s, elle ne passerait pas. Ultra reste « Ethernet
  seulement », comme prévu en §1.
- L'AV1 ne gagne rien sur le HEVC à la même barre (+1 à 4 ms, +20 à 50 % de
  débit, UM790Pro), et il casse sur le N95. Il ne sert pas de référence.

**Le 04/10 (UM790Pro, build `2ef56bfe` : `sctpburst=0` et `retrcut=3`
par défaut depuis ce build, à dire en comparant aux passes d'avant)** :
- **`6a7c3b43` est vérifié.** En « Auto » sur l'Arc, un stream AV1 monte à
  240 i/s comme un HEVC : décision prise vers 5-18 s, gardée. Le filet joue
  aussi en AV1 : une fois, une file de décodeur qui tient a fait redescendre la
  détection, qui a ensuite regagné 240 i/s. L'AV1 reste derrière : clic de
  43 ms contre 37, 40-46 Mbit/s contre 25-27.
- **Sous charge GPU** (`mw-gpu-load` à ~45 i/s, `4ba3f0d1`), deux passes par
  mode :

  | GPU chargé | Mode | Âge montré | E2E hôte → dessin | Avant la capture | Clic → drapeau |
  |---|---|---|---|---|---|
  | Arc | D | 126-127 ms | 38 ms | ~67 ms | 84-86 ms |
  | Arc | U | 129-137 ms | 39-52 ms | ~67 ms | 84-86 ms |
  | iGPU AMD | D | 163-164 ms | 13-14 ms | 137-158 ms | 135-137 ms |
  | iGPU AMD | U | 168-187 ms | 13 ms | 137-158 ms | 131-135 ms |

  Sous charge, c'est surtout la page de banc elle-même qui attend son GPU,
  avant la capture. Le stream n'ajoute que 13 ms (AMD) à 38-52 ms (Arc). U n'y
  change rien de net. Mais la fenêtre de charge était sur l'écran virtuel
  capturé, qui devient l'écran principal pendant le stream (corrigé dans
  `beeabde4`).
- **Cases refaites** (04/10, 17:04-17:25, build `ded56fc6`, fenêtre de charge
  sur un écran à part, deux passes par mode). L'écran virtuel de l'UM790Pro
  était entre-temps passé de 120 à 240 Hz, sans le banc : ce n'est pas tout à
  fait la même case que le matin.

  | GPU chargé (charge) | Mode | Âge montré | E2E hôte → dessin | Avant la capture | Clic → drapeau |
  |---|---|---|---|---|---|
  | Arc (30 i/s) | D | 192 ms | 48 ms | ~105 ms | 99 ms |
  | Arc (30 i/s) | U | 189 ms | 50 ms | ~105 ms | 96 ms |
  | iGPU AMD (47-52 i/s) | D | 107 ms | 23 ms | 70-75 ms | 93 ms |
  | iGPU AMD (47-52 i/s) | U | 118 ms | 28 ms | 70-75 ms | 97 ms |

  Le verdict ne change pas. Sous charge, la page de banc attend son GPU, U et
  D font jeu égal, et le stream reste la plus petite part de l'âge. Au même
  niveau, la charge tourne à 30 i/s sur l'Arc au lieu de 45 le matin : sa
  fenêtre, posée sur un écran physique, ne pèse plus pareil.

**Reste avant la porte U0** : l'iPad et RE9 sur l'écran de la RTX (avec
Bruno),
les TV (U0.3 quater), la borne Steam (U0.4, si Bruno l'accepte) et le
rapport U0.5.

## 7. Concrètement, pour l'utilisateur

Pendant le POC, rien ne change : Ultra est caché derrière deux clés de banc et
n'est jamais choisi seul. S'il gagne, il deviendra plus tard, dans un autre plan,
un mode « Ultra (LAN) » pour un appareil relié en Ethernet. L'image arriverait
plus tôt, surtout quand l'hôte a un GPU Intel ou AMD intégré, et sans image clé :
une perte ne ferait plus sauter l'image, elle flouterait quelques blocs pendant
une image. En échange, le débit passerait de 20-40 à 150-200 Mbit/s. Sur un PC
NVIDIA, il faut s'attendre à une égalité, voire une légère perte. S'il perd, on
saura pourquoi et de combien, et les gains trouvés en chemin hors codec (cadence
à 120 ou 240 Hz, présentateur, réglages du transport) iront à tout le monde. Les
TV passent au banc elles aussi : si le décodeur Ultra tourne assez vite sur leur
petit GPU, elles en profiteront ; sinon elles gardent leur décodeur matériel.

Déjà visible (U0.2) : le détail de la latence, dans les statistiques du stream,
gagne une ligne « Mesurée (hôte → dessin) ». C'est l'âge réel de l'image à
l'écran, pris d'un bout à l'autre. Si elle lit nettement plus que le total
au-dessus d'elle, une partie du trajet n'est chronométrée par personne. En Wi-Fi,
elle peut lire un peu faux, de la moitié de l'écart entre la montée et la
descente.

Déjà visible (U0.3, N95) : sur un portable modeste en Wi-Fi, forcer 120 i/s
dégrade tout. L'image a 130 à 320 ms de retard au lieu de 45 à 58, avec des
coupures de plusieurs secondes, et plus d'un clic sur deux reste sans réponse visible.
L'« Auto » d'aujourd'hui fait le bon choix sur cet appareil : il essaie de
monter, voit que ça ne tient pas, et reste à la cadence de l'écran. En Ethernet, sur un mini-PC (U0.3, UM790Pro), le même « Auto » monte à
240 i/s : l'image a 24 à 27 ms de retard et le clic s'affiche en 35 à 43 ms,
aussi bien que le réglage Ultra forcé.
