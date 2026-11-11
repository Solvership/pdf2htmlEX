# pdf2htmlEX – lokalna nadogradnja: 1:1 vizualno + ispravna struktura tablica

## Kontekst

Korisnik želi lokalno unaprijediti pdf2htmlEX (fork, `master`, v0.18.8.rc2, Poppler 24.06.1, FontForge 20230101).
Izlaz se koristi **i za gledanje (mora biti 1:1)** i **za strojnu obradu / kopiranje**. Problem: kad PDF ćelija
ima tekst u 2 retka, u HTML-u to postanu 2 nezavisna retka → pri parsiranju/kopiranju "jedan red tablice postane dva".

### Kako alat radi (analiza)
- `HTMLRenderer` je Poppler `OutputDev`. Tekst ide u HTML kao **apsolutno pozicionirani `<div class="t">` po vizualnoj liniji**
  (`HTMLTextPage` → `HTMLTextLine`, `white-space:pre`), fontovi se izvlače kroz FontForge u WOFF.
- Sva grafika (rubovi tablica, sjenčanje, slike) se renderira u **pozadinsku sliku** po stranici (`BackgroundRenderer/` Cairo→SVG ili Splash→PNG).
- Spajanje teksta u linije: `src/HTMLRenderer/state.cc` (`check_state_change`/`prepare_text_line`, pragovi `--heps/--veps`).
- `DrawingTracer` (`src/DrawingTracer.cc`) već presreće sve `stroke/fill/clip` putanje i znakove, a `CoveredTextDetector`
  već čuva **bbox svakog znaka** (`char_bboxes`, indeks = redoslijed crtanja; `HTMLLineState::first_char_index` veže liniju na znakove).
- **Tagged PDF struktura (StructTreeRoot, Table/TR/TD, MCID) se potpuno ignorira** – nema `beginMarkedContent` ni StructTree koda.
- `HTMLTextPage::optimize()` je prazan TODO.

**Zaključak:** vizualna vjernost je već vrlo dobra (kad se fontovi ugrade), ali HTML **nema nikakvu semantiku** –
ne postoje redovi/ćelije, samo linije teksta. Zato "red postane dva". To se ne rješava podešavanjem pragova,
nego **dodavanjem semantičkog sloja tablica** preko postojećeg 1:1 vizualnog sloja (vizualni izlaz ostaje identičan).

### Podrška za "moderne" PDF-ove (stanje)
| Područje | Stanje | Napomena |
|---|---|---|
| PDF 1.7 / 2.0, enkripcija | ✔ (Poppler 24.06) | može na noviji Poppler |
| Slike JPEG, JPEG2000 (openjp2), JBIG2, CCITT | ✔ | idu u pozadinsku sliku; kvaliteta ovisi o `--dpi`/`--bg-format svg` |
| Prozirnost, blend, shading | ✔ rasterizirano/SVG u pozadini | |
| Fontovi Type1/TrueType/CFF/OpenType | ✔ ugrađeni kao WOFF | |
| Type3 fontovi | ⚠ default → slika (nije selektabilno) | `--process-type3 1` eksperimentalno |
| Vertikalno pisanje (WMode) | ⚠ slika | |
| Forme / anotacije | ⚠ samo text field + radio | |
| Tagged PDF (struktura, tablice, alt-text) | ✘ | **glavni nedostatak** |
| Tablice kao `<table>` | ✘ | **glavni nedostatak** |

---

## Preporučeni pristup (fazno – svaka faza je samostalno upotrebljiva)

### Faza 0 – Lokalni build i test-infrastruktura
- Nema lokalnog Poppler/FontForge dev okruženja; postojeći `docker/Dockerfile.bookworm` klonira **upstream** s GitHuba.
  → Novi `docker/Dockerfile.dev` koji kopira **lokalni** izvorni kod i koristi `buildScripts/buildInstallLocallyApt`
  (sa `sed 's/sudo //'` kao u postojećem Dockerfileu) + `texlive`, `libreoffice`, `poppler-utils`, `chromium` za testove.
  Skripta `scripts/dev-build.sh` (build image, mount izvora, inkrementalni `make` u `build/`).
- Testni korpus `pdf2htmlEX/test/tables/`:
  - LaTeX (`.tex` kao postojeći `test/browser_tests/basic_text.tex`): tablice s rubovima, `p{}` stupci s višerednim ćelijama,
    `\multicolumn`/`\multirow`, tablica preko 2 stranice, tablica bez rubova.
  - LibreOffice → **tagged PDF** (`soffice --convert-to 'pdf:writer_pdf_Export:{"UseTaggedPDF":{"type":"boolean","value":"true"}}'`) iz istih tablica (.odt/.docx).
  - Za svaki: `expected.tables.json` (redovi/ćelije kako ih čovjek vidi).
- **Baseline vizualne regresije** prije ikakvih promjena: postojeći `test/browser_tests.py` + `test/compareTestImages`
  (headless browser screenshot vs. referenca). Pravilo: semantičke promjene **ne smiju promijeniti piksele**.

### Faza 1 – Brzi dobici vjernosti (bez novog koda ili minimalno)
- Preporučeni "fidelity" preset (dokumentirati / dodati `--preset fidelity`): `--bg-format svg`, `--process-type3 1`
  (provjeriti na korpusu), `--correct-text-visibility 2`, `--embed-external-font 1`, `--font-size-multiplier 4`, `--dpi 288` za PNG.
- Ispravak poznatih TODO-a ako ih korpus pokaže (npr. `"TODO: non-zero origins"` u `src/HTMLRenderer/text.cc:88`).

### Faza 2 – Semantički sloj tablica (srž rješenja)

**2a. Prikupljanje ravnala (rulings)** – `src/DrawingTracer.{h,cc}`
- U `do_path`/`stroke`/`fill` prepoznati vodoravne/okomite segmente i tanke ispunjene pravokutnike (debljina < ~2pt)
  i pune pravokutnike ćelija (pozadina); emitirati preko novog callbacka `on_rule_drawn(x0,y0,x1,y1)` u device prostoru
  (isti prostor kao `char_bboxes`). Napomena: tracer radi samo uz `ENABLE_SVG`/cairo → semantika zahtijeva cairo build (default je uključen).

**2b. Tagged PDF put (najpouzdaniji, ima prednost)** – `src/HTMLRenderer/` + novi `src/StructureMapper.{h,cc}`
- Implementirati `OutputDev::beginMarkedContent/endMarkedContent` u `HTMLRenderer` → stog MCID-ova; svakom znaku
  (globalni char index) pridružiti trenutni MCID.
- Pri učitavanju dokumenta (`HTMLRenderer::process`, `general.cc`) proći Poppler `StructTreeRoot`/`StructElement`
  (`Catalog::getStructTreeRoot()`): za `Table/THead/TBody/TR/TH/TD` izgraditi mapu `(page, MCID) → (table, row, col, rowSpan, colSpan)`
  (RowSpan/ColSpan iz atributa `Table` vlasnika).

**2c. Geometrijski put (netagirani PDF, tablice s rubovima)** – novi `src/TableDetector.{h,cc}`
- Na kraju stranice (prije `html_text_page.dump_text` u `endPage`): spojiti kolinearne segmente, klasterizirati x/y koordinate
  (tolerancija ~1–2 px), naći sjecišta → mreža; ćelija = pravokutnik omeđen s 4 strane (spojene ćelije kad unutarnji rub nedostaje → row/colspan).
- Tablica = povezana komponenta mreže s ≥2×2 ćelija (filtrirati okvire stranice, podcrtavanja).

**2d. Dodjela teksta ćelijama i dijeljenje linija** – `src/HTMLTextLine.{h,cc}`, `src/HTMLTextPage.{h,cc}`
- Za svaki znak linije (preko `first_char_index` + `char_bboxes` iz `CoveredTextDetector` – izložiti getter) odrediti ćeliju
  (središte bboxa u ćeliji; ili MCID→ćelija iz 2b).
- Ako jedna `HTMLTextLine` prelazi granicu ćelija (pdf2htmlEX je spojio susjedne ćelije u jednu liniju) → **podijeliti liniju**
  na granici (nova linija s istim stanjem i `x` pomaknutim za akumuliranu širinu; ponovo iskoristiti logiku iz `optimize()` koja već dijeli linije).
- `HTMLTextPage` dobiva model `Table{ id, cells[r][c]{rowspan,colspan, lines[]} }`.

**2e. Izlaz (vizual ostaje 1:1)** – opcije u `src/pdf2htmlEX.cc` + `src/Param.h`
- `--semantic-tables 0|1` (default 0 dok se ne stabilizira).
- Svaki `.t` div u tablici dobiva `data-tbl="p3t1" data-r="4" data-c="2"` (+ `data-rs`/`data-cs`) – ne mijenja izgled.
- `--tables-output none|json|html|both`: sidecar `<ime>.tables.json` i/ili `<ime>.tables.html` s pravim `<table>`;
  tekst višerednih ćelija spojen u jednu ćeliju (razmak ili `<br>` – opcija `--cell-line-join space|br`); spajanje rastavljenih riječi s crticom.
- Opcionalno skriveni pristupačni `<table class="sr-only">` u samoj stranici (za čitače ekrana / alate koji parsiraju HTML).
- **Kopiranje iz preglednika**: u `share/pdf2htmlEX.js.in` handler `copy` događaja – ako selekcija pokriva `.t` s `data-tbl`,
  u clipboard staviti `text/html` `<table>` + `text/plain` TSV → lijepljenje u Excel/Word daje ispravne redove.

### Faza 3 (opcionalno) – Tablice bez rubova i tekstualni tok
- Heuristika poravnanja stupaca (klasteri lijevih/desnih x-rubova linija, vertikalni "whitespace" kanali) za tablice bez linija.
- Popuniti `HTMLTextPage::optimize()`: grupiranje linija u odlomke izvan tablica (`data-para`), da i obični tekst ne bude "linija po linija".

### Faza 4 (opcionalno) – Nadogradnja ovisnosti
- Poppler 24.06.1 → najnoviji stabilni (izmjene API-ja popraviti u `src/`, praćenje u `PopplerReleases.md`), FontForge → najnoviji.
  Raditi nakon Faze 2 da regresije budu mjerljive korpusom.

---

## Kritične datoteke
- `pdf2htmlEX/src/DrawingTracer.{h,cc}` – prikupljanje ravnala
- `pdf2htmlEX/src/CoveredTextDetector.{h,cc}` – getter za `char_bboxes`
- `pdf2htmlEX/src/HTMLRenderer/HTMLRenderer.h`, `general.cc` (process/endPage, StructTree), `state.cc`, `text.cc` (MCID po znaku)
- `pdf2htmlEX/src/HTMLTextLine.{h,cc}`, `HTMLTextPage.{h,cc}` – dijeljenje linija, model tablice, data-atributi
- novo: `src/TableDetector.{h,cc}`, `src/StructureMapper.{h,cc}`, `src/TableWriter.{h,cc}` (JSON/HTML sidecar)
- `pdf2htmlEX/src/Param.h`, `src/pdf2htmlEX.cc` – nove opcije; `pdf2htmlEX/CMakeLists.txt` – novi izvori
- `pdf2htmlEX/share/pdf2htmlEX.js.in` – copy handler
- `docker/Dockerfile.dev`, `pdf2htmlEX/test/tables/*`

## Verifikacija
1. `docker build -f docker/Dockerfile.dev` i build prolazi; `pdf2htmlEX --version`.
2. Postojeći testovi (`test/runLocalTestsPython`, `test/test_output.py`, `test/browser_tests.py`) prolaze **bez promjena referentnih slika** s `--semantic-tables 0` **i** `1` (dokaz da je vizual i dalje 1:1).
3. Novi test `test/tables/test_tables.py`: za svaki PDF iz korpusa usporedi `*.tables.json` s `expected.tables.json`
   (broj redova/stupaca, tekst ćelija – višeredna ćelija = jedna ćelija, spans).
4. Ručno: otvoriti HTML u Chromeu, označiti tablicu, Ctrl+C → zalijepiti u LibreOffice Calc – broj redova odgovara PDF-u.
5. Usporedba vizualnog podudaranja s izvornikom: `pdftoppm -r 144` stranice vs. headless screenshot HTML-a (pixel diff ispod praga).
