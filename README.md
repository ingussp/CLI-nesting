# CLI-nesting — detaļu izvietošana loksnēs

Windows x64 konsoles lietotne divdimensiju detaļu izvietošanai loksnēs.
Visas detaļas, loksnes, aprēķina režīmu, GPU un izvades opcijas var norādīt
vienā `input.json`. Detaļu kontūras sastāv no punktiem; ir atbalstīti caurumi.
Rezultāti: JSON integrācijai, DXF atvēršanai CAD un SVG ātrai apskatei.

Projekta repozitorijs: [ingussp/CLI-nesting](https://github.com/ingussp/CLI-nesting).
Izpildfaila nosaukums ir **deepnestcpp.exe**; C++ bibliotēkas un vārdtelpas
saglabā DeepnestCPP nosaukumus. Šajā repo ir lietotnes pirmkods, Windows
būvēšanas skripti, piemēri un nepieciešamie trešo pušu galvenes faili/licences.

- [Pilns, palaižams input.json](input.json)
- [Pirmā varianta piemērs](examples/mode-first.json)
- [10 minūšu meklēšanas piemērs](examples/mode-timed.json)
- [Nepārtrauktās meklēšanas piemērs](examples/mode-continuous.json)
- [Īsā JSON uzziņa angliski](JSON_INPUT.md)

## Ātrā palaišana

1. Ja lejupielādēji pirmkodu, vispirms izpildi zemāk aprakstīto Windows kompilāciju.
   Ja saņēmi gatavu EXE, novieto to vienā mapē ar `input.json` un `run.cmd`.
2. Rediģē repo saknes vai gatavā komplekta `input.json`.
3. Palaid `run.cmd`: tas atrod EXE blakus skriptam vai `build-release/Release`.
   Gatavam komplektam terminālī var palaist arī `./deepnestcpp.exe`.
4. Apskati konsoles paziņojumu, `result.json`, `result.dxf` vai `result.svg`.
   Ja `output.openPreview` ir ieslēgts, Windows atvērs SVG ar noklusēto skatītāju.
5. Nepārtrauktu darbu apturi ar Ctrl+C, Ctrl+Break vai aizverot programmas konsoles logu.

Bez argumentiem programma meklē `input.json` pašreizējā darba mapē.
Komplekta `run.cmd` vispirms pāriet uz EXE mapi. Citu ievadi var izvēlēties
ar `./deepnestcpp.exe --input "C:/darbs/input.json"`.
JSON norādītie relatīvie izvades ceļi un `results` mape atrodas **blakus ievades JSON**.
EXE nav jāatrodas tajā pašā mapē, ja lieto `--input`.

## Trīs darba režīmi

| config.mode | Darbība | Kad beidzas | Ko saglabā |
|---|---|---|---|
| `first` | Aprēķina pirmo gatavo izvietojumu ar vienu compact stratēģiju | Pēc šīs stratēģijas | Vienu JSON un ieslēgtos DXF/SVG |
| `timed` | Atkārtoti izmēģina stratēģijas, detaļu secības un leņķu prioritātes | Pēc `timeLimitSeconds` vai lietotāja apturēšanas | Sesijas labāko variantu beigās |
| `continuous` | Turpina meklēt jaunus uzlabojumus bez kopēja laika limita | Līdz lietotāja apturēšanai | Katru uzlabojumu atsevišķos numurētos failos |

### A. Pirmais variants

```json
"config": {
  "mode": "first",
  "rotationStep": 90,
  "gpu": false
}
```

Šis ir noklusētais režīms. Programma mēģina izvietot visas detaļas,
vajadzības gadījumā pārejot uz nākamajām ievadītajām loksnēm, un tad atgriež
iegūto izvietojumu. Tā negaida papildu stratēģijas vai nejaušus atkārtojumus.
`trials` šajā režīmā nav ietekmes: tiek izmantota viena stratēģija.
Rotāciju pārbaudes un lokāla izvietojuma uzlabošana joprojām notiek.

“Pirmais variants” nenozīmē garantiju, ka visas detaļas ietilps:
apskati `unplacedCount`. Ja nepietiek vietas vai meklēšana neatrod derīgu pozīciju,
detaļas paliek neizvietoto sarakstā. Šajā režīmā `timeLimitSeconds` jābūt 0
vai laukam jābūt izlaistam.

### B. Labākais variants noteiktā laikā

```json
"config": {
  "mode": "timed",
  "timeLimitSeconds": 600,
  "continuousRoundSeconds": 30,
  "trials": 4,
  "threads": 12,
  "gpu": true
}
```

600 sekundes ir 10 minūtes. Programma meklē visu norādīto laiku, arī ja
visas detaļas izvietotas jau agrāk. Viena posma beigās sāk nākamo ar citu
detaļu secību un leņķu prioritāti. Labākais variants netiek zaudēts, ja
vēlāks mēģinājums ir sliktāks vai nepabeigts. Beigās saglabā un parāda labāko.

`continuousRoundSeconds` ierobežo vienu meklēšanas posmu. Pēdējam posmam pieejams
tikai atlikušais kopējais laiks. Kopējais termiņš aptver CPU, GPU un
sagatavošanas darbu pēc ievades nolasīšanas; tas nesākas no jauna katrai loksnei.
Ja limits ir ārkārtīgi īss, iespējams rezultāts ar visām detaļām neizvietotām.

Limits ir kooperatīvs: jau sākta OpenCL vai ģeometrijas operācija tiek droši
pabeigta. Tāpēc procesa faktiskā darbība var būt nedaudz ilgāka par limitu;
JSON nolasīšana, failu rakstīšana, resursu atbrīvošana un skatītāja atvēršana
arī aizņem laiku. Tas nav stingrs reāllaika procesa termiņš.

### C. Nepārtraukta meklēšana

```json
"config": {
  "mode": "continuous",
  "continuousRoundSeconds": 30,
  "trials": 4,
  "gpu": true
},
"output": {
  "json": "result.json",
  "dxf": true,
  "svg": true,
  "openPreview": false
}
```

Startējot šo režīmu, blakus ievades failam izveido vai **iztīra visu `results` mapi**.
Pirms atkārtotas palaišanas pārkopē vajadzīgos iepriekšējos rezultātus citur.
Ievades failu nedrīkst glabāt pašā `results` mapē.
`first` un `timed` režīmi šo mapi automātiski netīra.

Pirmais aprēķinātais variants izveido sākuma atskaites punktu.
Pēc tam saglabā tikai labākus variantus:

```text
results/
  result1.json
  result1.dxf
  result1.svg
  result2.json
  result2.dxf
  result2.svg
```

DXF un SVG rodas tikai tad, ja tie ieslēgti `output`. Failu nosaukumi šajā režīmā
vienmēr ir `resultN`; `output` norādītie nosaukumi neietekmē numerāciju.
Katras sesijas numerācija sākas ar 1. Vienādi vai sliktāki rezultāti neveido jaunus failus.
“Veiksmīgs rezultāts” šeit ir derīgs sākuma variants vai stingrs kvalitātes uzlabojums,
nevis katrs pabeigtais mēģinājums.

Darbs turpinās arī pēc visu detaļu izvietošanas un pēc posmiem bez uzlabojumiem.
`timeLimitSeconds` šajā režīmā netiek izmantots kā kopējais limits.
Ja ieslēgta automātiskā apskate, atver tikai pirmo SVG, lai neradītu jaunu logu
katram uzlabojumam. Jaunākos variantus atver no `results` mapes.

Ctrl+C un Ctrl+Break pieprasa drošu apturēšanu. Aizverot konsoles logu ar X,
Windows dod ierobežotu laiku procesa pabeigšanai. Programma mēģina saglabāt
labāku jau pārbaudītu variantu, bet ilga GPU operācija var nepabeigties šajā laikā.
Jau publicētie faili paliek pieejami. Task Manager piespiedu apturēšana vai
strāvas zudums nevar garantēt pašlaik aprēķinātā varianta saglabāšanu.

Failus vispirms pilnībā uzraksta pagaidu nosaukumos; tad publicē SVG/DXF un beigās JSON.
Galīgais JSON ir pazīme, ka rezultāta komplekts ir gatavs.
Pēc piespiedu apturēšanas var palikt pagaidu fails vai SVG/DXF bez atbilstoša JSON;
šādu komplektu neuzskati par pabeigtu rezultātu.

## Pilns ievades piemērs

Visi vadības iestatījumi ir šajā failā; papildu CLI parametri nav nepieciešami.

Repo saknes [input.json](input.json) ir pilnā opciju demonstrācija ar 24 detaļām:
fiksēts leņķis, atļauto leņķu saraksts, kopējais leņķu režģis ar nobīdi,
detaļas caurums, taisnstūra loksne un punktu loksne ar caurumu.
Tas pēc noklusējuma atrod pirmo variantu ar četriem kopējā režģa leņķiem,
ieslēdz GPU ar CPU rezerves režīmu un saglabā JSON, DXF un SVG.
`threads: 8` ir piemēra izvēle, nevis programmas noklusējums.

Pilnīguma dēļ šajā failā ir gan `rotations`/`rotationStep`, gan
`resolution`/`bitmapResolutionMm`, gan `step`/`bitmapSearchStepPx`, kā arī
`quantity`/`count` un `id`/`name` demonstrācijas. Ikdienas failā atstāj vienu
attiecīgā iestatījuma rakstības veidu; dublētus iestatījumus nemaini neatkarīgi.
`name` un `count` tiek izmantoti tikai tad, ja nav attiecīgi `id` un `quantity`.

Visas alternatīvās **vērtības** vienlaikus aktivizēt nevar. Piemēram, `mode`
var būt tikai viens režīms, `gpu` ir boolean **vai** objekts, `sheet` aizstāj
`sheets`, bet `angle` un `allowedAngles` pieder atsevišķiem detaļu ierakstiem.
Zemāk aprakstītas arī šīs formas; režīmu gatavie faili ir `examples` mapē.
Lai pilno failu mainītu:

- 10 minūtēm: `mode: "timed"`, `timeLimitSeconds: 600`, `continuous: false`.
- Nepārtrauktam darbam: `mode: "continuous"`, `continuous: true`, `timeLimitSeconds: 0`.
- 0.1° režģim: `rotationStep: 0.1` un `rotations: 3600`, vai izdzēs `rotations`.
- Tikai CPU: `gpu.enabled: false`, vai visu objektu aizstāj ar `"gpu": false`.

JSON nepieļauj komentārus vai komatu aiz pēdējā elementa. Lauku nosaukumi un
režīmu vērtības ir reģistrjutīgas. Raksti skaitļus bez pēdiņām un ar punktu
decimāldaļai; `true`/`false` ir loģiskās vērtības. Windows ceļus raksti kā
`"C:/darbs/result.json"` vai ar dubultotām slīpsvītrām `"C:\\darbs\\result.json"`.

Šis īsākais piemērs parāda 10 minūšu režīmu:

```json
{
  "units": "mm",
  "config": {
    "algorithm": "bitmap",
    "mode": "timed",
    "timeLimitSeconds": 600,
    "continuousRoundSeconds": 30,
    "rotationStep": 0.1,
    "resolution": 0.5,
    "threads": 12,
    "trials": 4,
    "step": 1,
    "curveTolerance": 0.3,
    "cacheRejects": true,
    "gpu": {
      "enabled": true,
      "device": -1,
      "fallbackToCpu": true,
      "batchSize": 65536
    }
  },
  "output": {
    "json": "result.json",
    "dxf": "result.dxf",
    "svg": "result.svg",
    "openPreview": true
  },
  "sheets": [
    {
      "id": "board",
      "quantity": 2,
      "points": [[0,0],[300,0],[300,200],[0,200]],
      "holes": [[[140,90],[160,90],[160,110],[140,110]]]
    }
  ],
  "parts": [
    {
      "id": "fixed",
      "quantity": 6,
      "angle": 45,
      "points": [[0,0],[30,0],[30,10],[0,10]]
    },
    {
      "id": "restricted",
      "quantity": 6,
      "allowedAngles": [0,90,180],
      "points": [[0,0],[30,0],[30,10],[10,10],[10,30],[0,30]]
    },
    {
      "id": "ring",
      "quantity": 4,
      "points": [[0,0],[30,0],[30,30],[0,30]],
      "holes": [[[10,10],[20,10],[20,20],[10,20]]]
    }
  ]
}
```

## Detaļas, loksnes un leņķi

| Lauks | Nozīme |
|---|---|
| `units` | Tikai `mm`; ja izlaists, pieņem milimetrus |
| `parts` | Netukšs detaļu tipu masīvs |
| `sheets` | Netukšs lokšņu masīvs; tā vietā var lietot vienu `sheet` objektu |
| `points` | Obligāti detaļai; loksnei aizstāj width/height. Kontūras punkti: `[x,y]`, `[x,y,z]` vai `{"x":x,"y":y}` |
| `holes` | Caurumu kontūru masīvs; katra kontūra ir punktu masīvs vai objekts ar `points` |
| `id` | Detaļas tipa/loksnes identifikators, teksts vai vesels skaitlis; rezultātā saglabājas kā `source`. Detaļu tipu ID jābūt unikāliem |
| `name` | Teksta alternatīva `id`, izmanto tikai tad, ja `id` nav. Ja nav abu, ģenerē `part_1`, `part_2` vai `sheet_1`, `sheet_2` |
| `quantity` | Fizisko kopiju skaits; vesels skaitlis, noklusēti 1. Detaļām 1–100000, loksnēm 1–1000; arī kopējie izvērstie skaiti nedrīkst pārsniegt šīs robežas |
| `count` | Tikai detaļām: `quantity` alternatīva ar tādām pašām robežām, izmanto tikai tad, ja nav `quantity`. Loksnēm lieto `quantity` |
| `angle` | Vienīgais atļautais detaļas leņķis grādos |
| `allowedAngles` | 1–3600 atļauto absolūto leņķu saraksts |
| `rotation` | Detaļas sākuma leņķa nobīde grādos, noklusēti 0; pieskaita kopējā režģa leņķiem. Nav fiksēta leņķa aizstājējs. Parseris pieņem arī loksnei, bet loksnes pagriešanai to neizmanto |
| `filename` | Neobligāts teksts, noklusēti tukšs; ģeometrijas metadati. Parseris pieņem detaļai un loksnei. Tas neielādē failu, neietekmē izvietojumu un pašreizējā rezultāta JSON netiek eksportēts |
| `width`, `height` | Tikai taisnstūra loksnei bez `points`: abi obligāti, pozitīvi izmēri mm līdz 1000000. Nosaka izmantojamo loksnes taisnstūri |
| `x`, `y` | Taisnstūra loksnes sākuma punkts mm, katrs noklusēti 0. Netiek izmantoti kopā ar loksnes `points`; detaļas pozīciju aprēķina programma |

Taisnstūra loksni var rakstīt īsāk: `"sheet": {"width":300,"height":200}`.
Loksnei var pievienot `x`, `y`, `holes`, `quantity`.
Atkārtots pēdējais punkts, kas sakrīt ar pirmo, nav obligāts.
`[x,y,z]` arī pieņem, bet Z ignorē. Tas nav 3D nesting: ievadei jau jābūt
projicētai kopējā XY plaknē. Līknes pirms ievades jāpārvērš punktu kontūrās.
Lietotne nenolasa detaļu kontūras tieši no DXF, STEP vai FreeCAD dokumenta.

Leņķi tiek pielietoti pret ievadītajiem punktiem, pretēji pulksteņrādītāja
virzienam ap (0,0), pirms pārvietošanas uz izvēlēto pozīciju.
`angle: 45` nozīmē tikai 45°. `allowedAngles: [0,90]` ļauj izvēlēties vienu
no diviem leņķiem. Tie aizstāj kopējo rotāciju režģi konkrētajai detaļai.
Var lietot arī, piemēram, 13.25°. -90 normalizē uz 270; 360 uz 0;
sarakstus sakārto un izņem dublikātus.

Bez šiem laukiem darbojas `rotation + kopējā režģa leņķis`.
Nekombinē `angle` ar `allowedAngles` vai kādu no tiem ar nenulles `rotation`.
Visām `quantity` kopijām ir vienāds ierobežojums; atšķirīgiem kopiju leņķiem
izmanto atsevišķus ierakstus ar atšķirīgiem `id`.
Loksnes automātiski netiek rotētas.

Caurumiem jābūt kontūras iekšpusē un savstarpēji nepārklājošiem.
Atbalstīts viens caurumu līmenis. Gan detaļu, gan lokšņu caurumi saglabājas
ģeometrijas pārbaudēs, izvades punktos, DXF un SVG.

Saknes `config` un `output` objekti ir neobligāti: izlaižot tos, izmanto
zemāk norādītos noklusējumus. `parts` un `sheets` masīviem jābūt netukšiem.
`sheet` ir viens objekts, piemēram, `"sheet": {"width":300,"height":200}`;
ja norādīti abi, parseris izmanto tikai `sheets`. Neizmanto abus vienā ievadē.

Kontūras ievadē vajag 3–20000 punktus. Secīgi vienādi punkti un atkārtots
noslēdzošais punkts tiek noņemti; pēc tam vajag vismaz trīs punktus un nenulles
laukumu. Ārējai kontūrai pēc caurumu atņemšanas jāpaliek pozitīvam materiāla
laukumam. Tukšs `holes: []` nozīmē, ka caurumu nav; tas ir arī noklusējums.
Lieto vienkāršas kontūras bez paškrustojumiem un punktus kontūras secībā.

Punktu x/y un visas leņķu/skaitliskās vērtības pieņem tikai galīgus skaitļus
ar absolūto vērtību līdz 1000000, papildus konkrētā lauka šaurākajām robežām.
Masīva `[x,y,z]` Z arī tiek pārbaudīts, bet netiek izmantots. Punkta objekts
izmanto tikai x/y; papildu Z lauks nerada augstumu. Kontūru apejas virzienu
nav obligāti vienādot — ārējo kontūru un caurumus atšķir to JSON atrašanās vieta.

Nezināmi lauki `config`, `config.gpu` un `output` objektos izraisa kļūdu.
Papildu saknes, detaļas, loksnes un punkta metadatus parseris var ignorēt;
tie nekļūst par aprēķina opcijām. Visas atbalstītās ievades opcijas ir šajā README.

## Visi aprēķina iestatījumi

| config lauks | Noklusējums | Diapazons un nozīme |
|---|---|---|
| `mode` | `first` | `first`, `timed`, `continuous` |
| `algorithm` | `bitmap` JSON ievadei | `bitmap` vai atsauces `nfp` |
| `timeLimitSeconds` | 0 | 0–86400; `timed` vajag pozitīvu vērtību; pieņem daļsekundes |
| `continuousRoundSeconds` | 30 | 0.01–86400; viena optimizācijas posma limits |
| `threads` | Datora CPU pavedienu skaits | 1–256; CPU darba budžets |
| `trials` | 2 | 1–4; stratēģijas vienā posmā; `first` vienmēr izmanto 1 |
| `rotations` | 4 | 1–3600 vienmērīgi izvietoti leņķi pilnā aplī |
| `rotationStep` | 90 | Alternatīva `rotations`; 0.1–360°, jādala 360 veselā skaitā |
| `resolution` | 1 | Pozitīvs milimetru skaits vienā pikselī; alias `bitmapResolutionMm` |
| `step` | 1 | 1–100000; smalkās meklēšanas minimālais solis pikseļos; alias `bitmapSearchStepPx` |
| `curveTolerance` | 0.3 | Nenegatīva kontaktu priekšlikumu vienkāršošanas pielaide mm |
| `cacheRejects` | true | Atcerēties nederīgās rastra pozīcijas |
| `gpu` | false | Boolean vai OpenCL konfigurācijas objekts |
| `spacing` | 0 | Pašlaik pieņem tikai 0; nenulles vērtība izraisa kļūdu |
| `continuous` | false | Vecais režīma alias; jaunai ievadei lieto `mode` |

### Kāpēc vajadzīgs katrs aprēķina iestatījums

- **`mode`** izvēlas darba ilgumu un saglabāšanas darbību. `first` der ātrai
  izvietošanai, `timed` — optimizēšanai ar noteiktu budžetu, `continuous` —
  ilgai meklēšanai ar uzlabojumu vēsturi. Automātiskā režīma izvēle bez šī
  lauka ir izskaidrota saderības sadaļā.
- **`algorithm`** izvēlas rastra meklētāju `bitmap` vai iepriekšējo no-fit
  polygon algoritmu `nfp`. Bitmap opcijas nedod tādu pašu funkcionalitāti NFP;
  jaunajiem režīmiem izvēlies `bitmap`.
- **`timeLimitSeconds`** nosaka kopējo timed optimizēšanas laiku sekundēs.
  600 ir 10 minūtes, 3600 — stunda. `first` izmanto 0; `continuous` šo kopējo
  limitu ignorē. Eksportēšana var notikt pēc aprēķina termiņa.
- **`continuousRoundSeconds`** dod vienam atkārtojumam iespēju pabeigt darbu,
  vienlaikus ļaujot sākt jaunu secību un stratēģiju. Pārāk maza vērtība var
  tērēt laiku atkārtotai sagatavošanai. Tā darbojas gan timed, gan continuous;
  `first` to neizmanto.
- **`threads`** ierobežo CPU meklēšanas darba pavedienu budžetu. Izlaižot,
  izmanto aparatūras norādīto loģisko procesoru skaitu (vismaz 1). Mazāka
  vērtība atstāj vairāk CPU citām programmām. GPU draivera un sistēmas pavedieni
  šajā budžetā neietilpst; ne visos aprēķina posmos visu budžetu var izmantot.
- **`trials`** nosaka, cik no četrām stratēģijām izmēģina vienā posmā:
  1 — compact; 2 — arī pair_rows; 3 — arī large_first; 4 — arī small_first.
  Stratēģijas var darboties paralēli atbilstoši threads budžetam. Vairāk
  stratēģiju palielina iespēju atrast citu izvietojumu, bet arī darbu un atmiņu.
  `first` un uzdevumiem ar mazāk nekā 6 detaļu kopijām izmanto vienu stratēģiju.
- **`rotations`** ir vienmērīgi sadalītu orientāciju skaits, nevis leņķis:
  4 nozīmē 0°, 90°, 180°, 270°; 3600 nozīmē ik pa 0.1°. Samazināšana
  būtiski samazina aprēķinu. Detaļas angle/allowedAngles šo režģi aizstāj.
- **`rotationStep`** ļauj to pašu norādīt grādos. Noklusētais četru rotāciju
  režģis atbilst 90°. 1° dod 360 orientācijas, 0.1° — 3600. Piemēram, 7°
  nav derīgs, jo 360/7 nav vesels skaitlis. Ar `rotations` jābūt saskaņotam.
- **`resolution` / `bitmapResolutionMm`** nosaka viena rastra pikseļa izmēru
  mm; diapazons ir `(0; 1000000]`. Mazāka vērtība ļauj smalkāk meklēt pozīcijas,
  taču palielina rastru un atmiņas patēriņu. Samazinot pikseļa malu uz pusi,
  vienāda laukuma rastrā ir aptuveni četras reizes vairāk pikseļu. Tā nav
  detaļu mērogošana un nav garantēta griešanas pielaide.
- **`step` / `bitmapSearchStepPx`** ir smalkās rastra meklēšanas minimālais
  translācijas solis pikseļos. Fiziskais solis ir step × resolution mm.
  Lielāks solis var paātrināt meklēšanu, bet izlaist derīgas šauras pozīcijas.
  Rupjās un kontaktu meklēšanas posmi var izmantot citus soļus.
- **`curveTolerance`** ir kontaktu priekšlikumu kontūru vienkāršošanas pielaide
  mm no 0 līdz 1000000. Mazāka vērtība saglabā vairāk sīku kontūras pazīmju
  priekšlikumu ģenerēšanai; lielāka var samazināt šo darbu. 0 neatļauj pozitīvu
  vienkāršošanas pielaidi. Oriģinālie eksporta punkti no tā nemainās;
  šī opcija nepārvērš DXF lokus par punktiem un nenosaka atstarpi starp detaļām.
- **`cacheRejects`** ieslēdz nederīgo rastra pozīciju kešu, lai tās atkārtoti
  nepārbaudītu tajā pašā aizpildījuma stāvoklī. Ieslēgts var ietaupīt aprēķinu,
  izslēgts samazina šī keša atmiņu. Tas ir boolean, nevis keša apjoma iestatījums.
- **`spacing`** ir rezervētais atstarpes lauks; pašlaik der tikai skaitlis 0.
  Programma nekompensē instrumenta diametru vai griezuma platumu.
- **`continuous`** ir boolean saderībai ar veciem ievades failiem.
  Ja `mode` norādīts, `continuous` jābūt true tieši continuous režīmā un false
  abos pārējos. Ja lieto tikai mode, šo dublējošo lauku var izlaist.
- **`gpu`** nosaka OpenCL sadursmju pārbaudes izmantošanu; visi tā apakšlauki
  un to darbība aprakstīti nākamajā sadaļā.

Alias pāru `resolution`/`bitmapResolutionMm` un `step`/`bitmapSearchStepPx`
atšķirīgas vērtības netiek īpaši noraidītas: pašreizējais parseris īso nosaukumu
apstrādā pēdējo, un tas uzvar. Nepaļaujies uz to — saglabā vienu nosaukumu
vai vienādas vērtības, kā pilnajā piemērā.

Ja ir gan `rotations`, gan `rotationStep`, tiem jāapraksta vienāds režģis.
`rotationStep: 0.1` nozīmē 3600 leņķus no 0 līdz 359.9°.
Smalkāka leņķa precizitāte nemaina telpisko `resolution`.

`nfp` ir iepriekšējais atsauces algoritms. Tas nepiedāvā jaunos timed/continuous,
GPU vai `angle/allowedAngles` režīmus; pretrunīga kombinācija izraisa kļūdu.
Šai lietotnes darbplūsmai izmanto `bitmap`.

## GPU un CPU

| config.gpu lauks | Noklusējums objektā | Nozīme |
|---|---|---|
| `enabled` | true | Ieslēgt OpenCL |
| `device` | -1 | Automātiski dot priekšroku diskrētai GPU; citādi ierīces indekss |
| `fallbackToCpu` | true | GPU kļūmes gadījumā turpināt CPU un paziņot iemeslu |
| `batchSize` | 65536 | 256–262144 GPU kandidāti vienā partijā |

- `enabled` ir boolean: false izslēdz GPU arī tad, ja pārējie GPU iestatījumi
  paliek objektā; true mēģina inicializēt ierīci. Tukšs objekts `{}` to ieslēdz.
- `device` ir vesels skaitlis no -1 līdz 1024. -1 izvēlas automātiski;
  0, 1 utt. ir tieši `--list-gpus` izdrukas indeksi, nevis Windows Task Manager
  numerācija. Manuāla izvēle noder datoram ar integrēto un diskrēto GPU.
  Konkrētajam indeksam jābūt pieejamo ierīču sarakstā.
- `fallbackToCpu` ir boolean: true ļauj turpināt, ja nav OpenCL, izvēlētās
  ierīces vai GPU darbībā rodas kļūme. False pieprasa GPU un šādu kļūmi
  padara par aprēķina kļūdu; tas ir noderīgi, ja gribi pamanīt neaktīvu GPU.
- `batchSize` ir vesels skaitlis — vienā paketē GPU nosūtīto kandidātu limits.
  Lielākas partijas var samazināt daudzu mazu palaišanu izmaksas, bet palielina
  buferus un vienas operācijas aizkavi. Faktiskā partija var būt mazāka.

`gpu: true` ir īsā forma automātiskai izvēlei ar CPU rezerves režīmu.
`gpu: false` neielādē OpenCL. Ierīču sarakstu apskati ar
`./deepnestcpp.exe --list-gpus`, izvēlēto indeksu ieraksti JSON.
Vajadzīgs videokartes draiveris ar OpenCL. CUDA Toolkit un citu nesting projektu
DLL nav vajadzīgi; draivera `OpenCL.dll` netiek pievienots komplektam.

GPU pārbauda daudzu rastra kandidātu sadursmes. CPU sagatavo ģeometriju,
kontaktus, novērtē pozīcijas, precīzi pārbauda kontūras un pieņem izvietojumus.
Katra jau izvietota detaļa ietekmē nākamo, tāpēc šīs darbības nav pilnībā
paralēlas. 100% CPU/GPU slodze nav garantēta un pati par sevi nenozīmē
labāku rezultātu. Darbs, ko atrisina kontaktu priekšlikumi, var neizsaukt GPU
kodolu vispār. Mazus piemērus GPU palaišanas izmaksas var palēnināt.

## Izvades iestatījumi

Visi šie lauki ir saknes objektā `output`, līdzās `config`, `parts` un `sheets`.

| output lauks | Noklusējums | Nozīme |
|---|---|---|
| `json` | `"result.json"` | Rezultāta JSON ceļš; JSON izvade ir obligāta |
| `dxf` | false | `true` → result.dxf; false izslēdz; var norādīt ceļa tekstu |
| `svg` | false | `true` → result.svg; false izslēdz; var norādīt ceļa tekstu |
| `openPreview` | false | Atvērt SVG noklusētajā Windows skatītājā; vajag ieslēgtu `svg` |

JSON ceļi ir relatīvi ievades mapē vai absolūti. Trūkstošas izvades apakšmapes
first/timed režīmā tiek izveidotas. Neizmanto vienu failu ievadei un izvadei
vai vairākiem formātiem; programma šādas sakritības noraida.
SVG ir vienkāršs vizuāls pārskats ar loksnēm, krāsainām detaļām un caurumiem.
Uzvedot peli uz detaļas, skatītājs var parādīt tās ID un leņķi.
Ražošanai un integrācijai izmanto pilnos JSON/DXF datus.

`json` vajadzīgs integrācijai un pilnajam izvietojuma aprakstam; to nevar
izslēgt ar false. `dxf` ieslēdz CAD apmaiņas failu, `svg` — attēlu ātrai
apskatei. Abu boolean forma true lieto noklusēto faila nosaukumu **ievades
mapē**, nevis automātiski atvasina nosaukumu no `output.json`. Ceļa teksta forma
ļauj katram formātam izvēlēties savu atrašanās vietu. Tukšs ceļš nav derīgs.
`openPreview` neatver JSON vai CAD — tas atver tikai SVG. False ir piemērots
automatizētai palaišanai, kur papildu logs nav vajadzīgs. First/timed režīmā
esošie izvades faili tiek pārrakstīti; vēsturei izmanto continuous vai jaunus ceļus.

Nepārtrauktās meklēšanas mape ir fiksēta `results` ievades mapē. Saites un
Windows junction mapes tās iekšpusē tiek noraidītas pirms tīrīšanas.
Procesa slēdzene neļauj otrai sesijai tajā pašā mapē izdzēst aktīvās sesijas
rezultātus. Vienlaicīgām sesijām izmanto atsevišķas ievades mapes.

## Rezultāta JSON lasīšana

- `placed`, `unplacedCount`: izvietoto un neizvietoto fizisko kopiju skaits.
- `sheets[].parts`: detaļas katrā izmantotajā loksnē.
- Detaļas `id` ir unikāls kopijas skaitlis; `source` saglabā ievades tipa ID.
- `x`, `y`, `rotation`: pārvietojums un leņķis pret ievadīto kontūru.
- `points`, `holes`: **jau transformētas absolūtās koordinātas**.
  Tās atkārtoti nerotē un nepārvieto.
- `unplaced`: visas atlikušās kopijas, arī tās, kuras termiņa dēļ vēl nepaspēja mēģināt.
- `mode`, `timeLimitSeconds`, `timeLimitReached`, `stopReason`: meklēšanas režīms un pārtraukšanas iemesls.
- `stopReason`: completed, time_limit vai user_stop. Continuous failā tas
  raksturo konkrēto variantu, nevis nozīmē, ka visa sesija jau beigusies.
- `timingMs`: kopējais meklēšanas/orķestrācijas laiks; bez izvades failu rakstīšanas.
- `searchIteration`: posms, kurā atrasts saglabātais variants, sākot no 0.
- `trials`, `startedTrials`, `selectedTrial`, `workersUsed`,
  `proposalWorkersPerTrial`: stratēģiju un pavedienu diagnostika.
- `gpu.requested/used/device/batches/candidates/fallbackReason`: GPU diagnostika.
  used=true nozīmē, ka tiešām palaists GPU kodols. Timed gala GPU skaitītāji aptver
  visus posmus; continuous uzlabojuma failā tie raksturo kandidāta stratēģiju.
- `usedSheetWasteArea`: neizmantotais materiāls izmantotajās loksnēs mm²,
  neieskaitot lokšņu caurumus un pilnīgi neizmantotas loksnes.
- `occupiedBoundsArea`, `compactWasteArea`: rastra ietverošo taisnstūru
  laukums un neizmantotais laukums tajos.
- `utilisation`: vēsturiskais procentu rādītājs pret stratēģijā apstrādāto
  lokšņu materiāla laukumu; tas nav vienīgais kvalitātes kritērijs.

## Kā tiek atrasts un izvēlēts labākais variants

Bitmap algoritms veido loksnes aizpildījuma rastru, izmēģina robežu un kontūru
kontaktu pozīcijas, pārbauda sadursmes, vajadzības gadījumā paplašina meklēšanas
apgabalu un veic smalkāku meklēšanu. Pieņemto detaļu izvietojumu pārbauda arī
ar oriģinālo ģeometriju. Kontūru vienkāršošana palīdz izveidot priekšlikumus,
bet nemaina eksportēto detaļu formu.

Pirmajā optimizācijas posmā stratēģijas ir compact, pair_rows, large_first,
small_first; pieejamo skaitu nosaka trials. Ļoti maziem darbiem ar mazāk nekā
6 detaļām pietiek ar vienu stratēģiju. Nākamajos posmos maina detaļu secību un
atļauto leņķu izmēģināšanas prioritāti. Tie ir heuristiski mēģinājumi, nevis
globālā optimuma pierādījums.

Timed/continuous režīmi salīdzina rezultātus secīgi:
1. Mazāk neizvietotu detaļu.
2. Mazāks usedSheetWasteArea.
3. Mazāks compactWasteArea.

Tātad rezultātu nevar “uzlabot”, vienkārši izmetot detaļas.
Ja visas tās pašas detaļas jau atrodas tajās pašās loksnēs, kopējais atkritumu
laukums ir nemainīgs. Pārkārtošana tad uzlabo kompaktumu un atlikumu formu,
nevis kopējo atkritumu kvadrātmilimetru skaitu. Skaitliski līdzvērtīgi rezultāti
netiek uzskatīti par uzlabojumiem.

## Ātrdarbība un robežas

- Ja detaļas drīkst rotēt tikai dažos leņķos, norādi allowedAngles.
- Sāc ar rotationStep 90 vai 1; 0.1 var ievērojami palielināt darbu.
- Mazāka resolution uzlabo telpisko precizitāti, bet prasa vairāk atmiņas un aprēķinu.
- Lielāks trials dod vairāk variantu, taču arī lielākas izmaksas.
- Virs 64 atļautajiem leņķiem izmanto papildu CPU palīgus stratēģijas ietvaros.
- Ja īss posms beidzas tikai ar sagatavošanu, palielini continuousRoundSeconds.
- Lielāka GPU partija ne vienmēr ir ātrāka; salīdzini laiku un izvietojuma kvalitāti.

Ievades robežas: JSON līdz 64 MiB, līdz 100000 detaļu kopijām, 1000 lokšņu
kopijām, 20000 punktiem kontūrā; koordinātu absolūtā vērtība līdz 1000000 mm.
Atmiņas robežas atsevišķi aizsargā rastru. Pikseļu kešatmiņai paredzēti ap
64 MiB uz stratēģiju/loksni, nederīgo pozīciju kešam līdz 32 MiB. Ģeometrija
un citi dati ir papildu izmaksas. GPU masku buferis ierobežots līdz 512 MiB
un ierīces pieļaujamajam apjomam.

Nav ražošanas atstarpes/kerf kompensācijas: spacing pašlaik jābūt 0.
Rastra precizitāte var izslēgt zem pikseļa izmēra ietilpšanu.
Nav garantēts globāli optimāls izvietojums vai turpmāki uzlabojumi.

## Biežas situācijas

| Situācija | Rīcība |
|---|---|
| Logs uzreiz aizveras | Palaid run.cmd vai no termināļa un izlasi kļūdu |
| Nav pirmā rezultāta ļoti īsā posmā | Palielini continuousRoundSeconds; sagatavošana arī patērē laiku |
| GPU used=false | Iespējams, kontakti atrisināja darbu bez GPU; apskati fallbackReason |
| GPU nav atrasta | Atjaunini draiveri, pārbaudi --list-gpus vai atļauj fallbackToCpu |
| Rezultāts satur neizvietotas detaļas | Pārbaudi lokšņu izmērus, caurumus, leņķus, resolution un laika limitu |
| Vecie rezultāti pazuda | Continuous režīms apzināti tīra results katrā jaunā sesijā |
| Results directory already in use | Apturi otru sesiju vai izmanto citu ievades mapi |
| SVG neatveras automātiski | Atver saglabāto SVG pārlūkā; pārbaudi Windows failu asociāciju |
| FreeCAD nelasa DXF | Pārbaudi, ka atver tieši DXF, nevis pārsauktu JSON |
| “Unknown ... option” | Pārbaudi JSON lauka nosaukumu un atrašanās vietu |

Iziešanas kods 0 nozīmē veiksmīgu aprēķinu vai drošu lietotāja apturēšanu,
arī daļēja rezultāta gadījumā. Kods 1 nozīmē ievades, failu vai aprēķina kļūdu.
Piespiedu Windows procesa izbeigšana var dot citu OS noteiktu kodu.

## Iepriekšējo konfigurāciju saderība

Ja mode nav norādīts: continuous=true izvēlas continuous; citādi pozitīvs
timeLimitSeconds izvēlas timed; bez tiem izmanto first.
Pretrunīgs mode un continuous tiek noraidīts. **Laika režīms tagad optimizē
visu norādīto laiku**, nevis apstājas pēc sākotnējo stratēģiju pabeigšanas.

### Visas JSON lietotnes komandrindas opcijas

| Arguments | Vērtība un darbība |
|---|---|
| `--input path` | Ievades JSON; noklusēti `input.json` darba mapē |
| `--output path` | Pārraksta JSON izvades ceļu; `.dxf` paplašinājumam izvada DXF un tāda paša nosaukuma JSON |
| `--dxf path` | Ieslēdz DXF un pārraksta tā ceļu |
| `--threads N` | Pārraksta config.threads, vesels skaitlis 1–256 |
| `--trials N` | Pārraksta config.trials, vesels skaitlis 1–4; first tāpat izmanto vienu |
| `--rotations N` | Pārraksta kopējā režģa rotāciju skaitu, vesels skaitlis 1–3600 |
| `--list-gpus` | Parāda OpenCL GPU indeksus, nosaukumus, ražotāju un atmiņu MiB, pēc tam beidz darbu; ievades fails nav vajadzīgs |
| `--help`, `-h` | Parāda īso lietošanas palīdzību un beidz darbu |

JSON vispirms tiek pilnībā pārbaudīts, pēc tam piemēro CLI pārrakstījumus.
Tātad argumenti neizlabo savstarpēji pretrunīgu JSON konfigurāciju.
GPU, režīms, laika limits un SVG jānorāda JSON; tiem nav atsevišķu CLI slēdžu.
CLI ceļi ir relatīvi procesa darba mapei; JSON ceļi — ievades mapei.
--output layout.dxf raksta īstu DXF un blakus layout.json.
Lieto vienu konfigurācijas avotu ikdienas darbā, lai izvairītos no nejaušiem pārrakstījumiem.

## Windows kompilācija

Nepieciešams CMake 3.21 vai jaunāks gatavajiem presets (tiešam CMake minimums
ir 3.20), Visual Studio 2022 ar **Desktop development with C++**, MSVC x64,
Windows SDK un Git atkarības lejupielādei. Kompilators izmanto C++20.
Repo `build-release.ps1` sagatavo Release būvējumu:

```powershell
git clone https://github.com/ingussp/CLI-nesting.git
cd CLI-nesting
./build-release.ps1
./run.cmd
```

Skripts palaiž CMake konfigurēšanu un paralēlu kompilāciju. Rezultāts:
`build-release/Release/deepnestcpp.exe`. `run.cmd` izmanto repo saknes input.json.
Tieša palaišana no repo saknes:

```powershell
./build-release/Release/deepnestcpp.exe --input ./input.json
./build-release/Release/deepnestcpp.exe --list-gpus
```

Alternatīvi bez skripta:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release --parallel
```

MSVC runtime noklusēti piesaista statiski (`DEEPNEST_STATIC_RUNTIME=ON`),
lai lietotnes izplatīšanai nebūtu atsevišķi jāpievieno MSVC runtime DLL.
To var izslēgt konfigurēšanā ar `-DDEEPNEST_STATIC_RUNTIME=OFF`, ja būvēšanas
videi nepieciešams dinamisks runtime. OpenCL draiveris joprojām ir vajadzīgs GPU.
Vecais demonstrācijas izpildfails `deepnestcpp_demo.exe` nav JSON lietotne;
ikdienas darbam izmanto `deepnestcpp.exe` un šeit dokumentētos argumentus.

Pirmajā konfigurēšanā CMake lejupielādē Clipper2 1.5.4. Ja tā pirmkods jau ir
pieejams lokāli, var konfigurēt bez šīs lejupielādes:

```powershell
cmake --preset windows-release -DFETCHCONTENT_SOURCE_DIR_CLIPPER2=C:/deps/Clipper2
cmake --build --preset windows-release --parallel
```

Norādītajā mapē jābūt Clipper2 repo saknei ar `CPP` apakšmapi un atbilstošo
1.5.4 versiju. Šajā izstrādes posmā kompilē **tikai Windows x64**.
Linux būvējums paredzēts vēlāk; OpenCL izvēle saglabā šādas pārnešanas iespēju.

## Projekta uzbūve un bibliotēkas

- demo/json_main.cpp: JSON CLI, režīmu izvēle, signāli un izvade.
- src/json_io.cpp: ievades pārbaudes un rezultāta JSON.
- src/bitmap_nesting.cpp: izvietošana, stratēģijas, rastrs un CPU/GPU sadarbība.
- src/continuous_nesting.cpp: timed/continuous atkārtojumi un labākā varianta izvēle.
- demo/continuous_results.hpp: numurētā izvade, procesa slēdzene un droša mapes tīrīšana.
- demo/svg_preview.hpp: punktu ģeometrijas SVG priekšskatījums.
- src/gpu_bitmap.cpp: OpenCL ierīce, buferi un kodoli.
- src/geometry.cpp, src/nfp.cpp: precīzās ģeometrijas darbības.

Clipper2 1.5.4 tiek nodrošināts ar CMake FetchContent (Boost Software License 1.0).
Sākotnējai lejupielādei nepieciešams tīkls, ja nav lokālu atkarību.
nlohmann/json 3.11.3 ir repozitorijā ar MIT licenci.
Khronos OpenCL-Headers v2024.10.24 ir repozitorijā ar Apache-2.0 licenci.
OpenCL izpildbibliotēku nodrošina GPU draiveris.
Saglabā trešo pušu licences, izplatot būvējumus. Citu nesting projektu DLL
šī lietotne neizmanto.
