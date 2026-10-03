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
