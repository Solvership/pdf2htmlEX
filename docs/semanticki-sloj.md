# Semantički sloj: tablice i odlomci (lokalna nadogradnja)

pdf2htmlEX tekst prikazuje kao apsolutno pozicionirane retke (`<div class="t">`), a rubove
tablica crta u pozadinsku sliku. Vizualno je to vrlo vjerno, ali HTML nema strukturu: ćelija
čiji se tekst u PDF-u prelama u dva retka postaje dva neovisna retka teksta. Zato pri
parsiranju ili kopiranju **jedan red tablice postane dva**.

Ova nadogradnja dodaje **semantički sloj** preko vizualnog izlaza. Vizualni izlaz ostaje
nepromijenjen, što je provjereno:

* uz `--semantic-tables 0` (zadano) HTML je **bajt-identičan** originalu,
* uz `--semantic-tables 1` render u Chromiumu je **pikselski identičan** originalu
  (`scripts/visual_compare.py`, na testnim PDF-ovima i na 14 stranica s tablicama
  stvarnog godišnjeg izvješća od 170 stranica).

## Brzi početak

```bash
# vizualno najvjernije + tablice + sve izlazne datoteke
pdf2htmlEX --preset semantic dokument.pdf

# ili ručno
pdf2htmlEX --semantic-tables 1 --semantic-output attrs,inline,json,html dokument.pdf
```

Rezultat:

| Datoteka / element | Sadržaj |
|---|---|
| `dokument.html` | Isti vizualni izlaz + `data-*` atributi na tekstu + skriveni semantički HTML po stranici |
| `dokument.semantic.json` | Sve tablice (ćelije, spanovi, bbox, nastavci preko stranica) i odlomci |
| `dokument.tables.html` | Samo tablice kao prave `<table>`. Tablice koje se nastavljaju preko stranica su spojene, a ponovljeno zaglavlje izbačeno |

## Opcije

| Opcija | Zadano | Opis |
|---|---|---|
| `--semantic-tables 0\|1` | 0 | Uključuje semantički sloj |
| `--table-detect LISTA` | `tagged,ruled,hrules` | Detektori: `tagged`, `ruled`, `hrules`, `stream` ili `all` |
| `--semantic-output LISTA` | `attrs,inline,json` | Izlazi: `attrs`, `inline`, `json`, `html` |
| `--semantic-paragraphs 0\|1` | 1 | Grupira tekst izvan tablica u odlomke |
| `--semantic-spans 0\|1` | 0 | Retke koji miješaju ćelije omata u `<span data-*>` (vidi niže) |
| `--cell-line-join space\|br` | space | Kako se spajaju retci ćelije u semantičkom HTML-u |
| `--dehyphenate 0\|1` | 0 | `elektro-` + `energetski` → `elektroenergetski` (zadano: `elektro-energetski`) |
| `--preset fidelity\|semantic` | – | Grupe opcija (stavite ga **prije** ostalih opcija koje želite pregaziti) |

`--preset fidelity` postavlja SVG pozadinu i Type 3 fontove kao tekst. Izbor je napravljen
mjerenjem (`scripts/fidelity_metric.py`, raster PDF-a naspram rendera HTML-a):

* PNG pozadina pri uvećanju 4× odstupa 4–8× više od SVG pozadine.
* `--correct-text-visibility 2` i viši `--dpi` dali su **lošiji** rezultat, pa nisu dio preseta.

## Detektori tablica

1. **tagged**: tagirani PDF (Word/LibreOffice izvoz s „tagged PDF”, PDF/UA). Struktura se
   čita iz stabla strukture PDF-a (`Table/TR/TH/TD`, `RowSpan/ColSpan`) i povezuje s tekstom
   preko MCID oznaka. Ovo je **100 % pouzdano**: radi i bez ikakvih linija, a TH ćelije su
   označene kao zaglavlje.
2. **ruled**: tablice s mrežom linija. Okomite i vodoravne linije, uključujući tanke ispunjene
   pravokutnike i rubove osjenčanih ćelija, slažu se u mrežu. Prepoznaju se spojene ćelije
   (colspan/rowspan). **Ćelija s više redaka teksta ostaje jedna ćelija.**
3. **hrules**: tablice sa samo vodoravnim linijama (booktabs, `border-bottom` po retku, zebra
   sjenčanje, financijski izvještaji). Stupci se određuju iz razmaka u tekstu, a retci iz linija
   i „sidrenog” stupca. Pokriva i tipičan izgled financijskih izvještaja i bilješki:
   * linije postoje samo ispod brojčanih stupaca: **nazivi redaka** lijevo od linija postaju prvi
     stupac, a **brojevi bilješki** („Bilješka”) zaseban stupac;
   * zaglavlje i prvi redovi **iznad prve linije** (kad linije odvajaju samo zbrojeve), uključujući
     naslove stupaca u više redaka i naslove skupina preko više stupaca („Prihodi segmenta”);
   * red s jedinicom („(u tisućama eura)”) i redovi zbrojeva bez naziva su zasebni redovi;
   * dvije tablice iste širine razdvojene rečenicom („… kako slijedi:”) ili naslovom ostaju
     dvije tablice;
   * male tablice istih stupaca jedna ispod druge, svaka sa svojim zaglavljem, ostaju zasebne
     tablice: red zaglavlja koji nakon redova s brojevima ponavlja zaglavlje tablice (iste
     tekstove ili godine) započinje novu tablicu;
   * naslov centriran iznad skupine stupaca, između njihovih vrijednosti (npr. „2025.” iznad
     „Brend | Goodwill”, ili „(u tisućama eura)”), proteže se preko cijele skupine (colspan).
4. **stream** (samo uz `all`): tablice bez ikakvih linija, iz poravnanja teksta. Ovo je
   heuristika, pa je isključena dok je ne zatražite.

**Pravilo sidrenog stupca:** novi red počinje kada prvi (lijevi) stupac s tekstom ima novi
redak teksta i uz njega postoji tekst u barem još jednom stupcu. Redak teksta koji je sam u
prvom stupcu nastavak je prelomljenog teksta, osim ako počinje **velikim slovom**: tada je to
naslov odjeljka („Kratkotrajna imovina”) ili prvi redak naziva čiji su brojevi u zadnjem retku
(sljedeći redak koji počinje malim slovom ga dovršava). Naziv koji se već prelomio u drugi
redak širok je kao njegov stupac, pa nije naslov odjeljka: njegov zadnji redak (s brojevima)
smije počinjati i velikim slovom („Neto knjigovodstvena / vrijednost / Na 31. prosinca”), osim ako
prelomljeni dio završava točkom ili dvotočkom (tada je to cjelovit naslov). Brojevi se ne prelamaju: broj ispod
broja u istom stupcu uvijek je novi red. Ovo se koristi samo tamo gdje linije ne razdvajaju
retke.

**Što se ne smatra tablicom:**
* **grafikoni**: stupci grafikona su obojeni pravokutnici i daju mrežu, ali ćelije su većinom
  prazne; linije rešetke grafikona imaju oznake osi („400”, „300”) točno na svojoj visini;
* **nevidljivi crteži**: bijele pozadine ćelija (izvoz iz Excela) i bijele linije na bijeloj
  stranici. Bijela ploha uz obojenu (zebra tablica) se zadržava;
* dijelovi crteža izvan clip-područja (npr. linije rešetke grafikona „ispod” teksta pokraj);
* okvir oko dijela tablice (u ćeliji bi bilo više brojeva jedan ispod drugog).

Okomiti (rotirani) tekst, npr. u zaglavljima, čita se u svom smjeru, a indeksi i eksponenti
(„tCO₂e”) ostaju u svom retku.

Tablice preko više stranica se povezuju (`continued_from`, `group`). Ponovljeno zaglavlje
na sljedećoj stranici prepoznaje se kao `repeated_header`.

## Format anotacija u HTML-u

* Redak teksta koji je cijeli u jednoj ćeliji:
  `<div data-tbl="p1t0" data-r="2" data-c="1" class="t ...">`
* Redak teksta u odlomku: `<div data-para="p1p3" ...>` (+ `data-tag="h1"` za naslove iz tagiranog PDF-a)
* Redak koji **miješa** više ćelija (pdf2htmlEX često spoji cijeli red tablice u jedan `div`):
  `data-sem="t:p1t0:4:2@0-4 t:p1t0:4:3@5-21"`, tj. rasponi znakova u `textContent` tog `div`-a
  (UTF-16 pomaci, kao u JavaScriptu). Sadržaj retka se ne dira, zbog pikselske identičnosti.
* Uz `--semantic-spans 1` takvi se retci umjesto toga omataju u `<span data-tbl=… data-r=… data-c=…>`.
  To je praktičnije za parsere, ali Chrome zbog dodatnih inline granica na transformiranom tekstu
  može pomaknuti antialiasing za djelić piksela.
* Skriveni semantički sadržaj stranice (redoslijed čitanja po XY-cut algoritmu, višestupčani
  prijelom se čita stupac po stupac):
  ```html
  <div class="pdf2htmlEX-semantic" data-page-no="1" hidden>
    <p data-para="p1p0">…</p>
    <table data-tbl="p1t0" data-source="ruled"><tr><td data-r="0" data-c="0">…</td>…</tr></table>
  </div>
  ```

## Kako vidjeti prepoznate tablice

Semantički sloj je **namjerno nevidljiv**: stranica izgleda točno kao PDF. U `dokument.html`
postoje tri dijela:

* vidljivi retci teksta (`div.t`) s atributima `data-tbl`/`data-r`/`data-c` ili `data-sem` (kojoj
  ćeliji pripada koji dio retka);
* po stranici skriveni `<div class="pdf2htmlEX-semantic" hidden>` s pravim `<table>` i `<p>`.
  `hidden` znači da ga preglednik ne prikazuje; služi za kopiranje, čitače ekrana i strojnu
  obradu (npr. `document.querySelectorAll('.pdf2htmlEX-semantic table')`);
* `pdf2htmlEX-semantic.js` za kopiranje i prikaz tablica.

Da biste u pregledniku vidjeli što je prepoznato, pritisnite tipku **T** (ili otvorite
`dokument.html#tables`). Preko stranice se iscrtava okvir svake tablice s oznakom
(`p104t0 · 28×10` = stranica 104, tablica 0, 28 redova × 10 stupaca) i obojena ćelija za svaki
dio teksta (prijeđite mišem preko ćelije za njezin red i stupac). Ponovni pritisak na T sakriva
prikaz. Bez toga se ništa ne iscrtava.

Za pregled samo tablica otvorite `dokument.tables.html`.

## Kopiranje iz preglednika

Kad je semantički sloj uključen, u HTML se ugrađuje `pdf2htmlEX-semantic.js`. Označite dio
tablice i pritisnite Ctrl+C: u međuspremnik ide TSV (`text/plain`) i prava `<table>`
(`text/html`). Lijepljenje u Excel, LibreOffice Calc ili Word daje **jedan red po redu tablice**.
Retci istog odlomka spajaju se razmakom umjesto prijeloma retka.

## Ograničenja

Isprobano na stvarnom dokumentu (godišnje izvješće, InDesign, 170 stranica, 90 tablica:
mreže linija, zebra tablice, financijski izvještaji i bilješke, tablice preko stranica,
rotirana zaglavlja, grafikoni). Na njemu nema poznatih pogrešaka u tablicama. Općenita
ograničenja:

* Pravilo velikog slova je heuristika: naziv u **jednom** retku bez brojeva koji počinje velikim
  slovom smatra se naslovom odjeljka. Ako mu se drugi redak (s brojevima) također piše velikim
  slovom (osobno ime, kratica), dobit će se dva reda.
* **stream** detektor je heuristika. Testiran je i ne prepoznaje tablice u običnom tekstu
  ni u dvostupčanom prijelomu, ali na neobičnim rasporedima može pogriješiti.
* Kod tablica bez linija između redaka, red u kojem je popunjen samo prvi stupac spaja se s
  prethodnim redom (posljedica pravila sidrenog stupca).
* Ako PDF za glif nema podatak o Unicode znaku (npr. Type 3 bitmap fontovi s imenima `/a51`),
  znak se ne može vratiti (`` umjesto `Š`). To ne može ni Acrobat. Gdje Poppler zna znak,
  semantički tekst koristi njega. Glif bez Unicodea koji ništa ne crta (razmak u fontu s
  neispravnom ToUnicode tablicom, čest u InDesign izvozu) u semantičkom tekstu je razmak;
  vizualni HTML i dalje sadrži znak iz privatnog područja, kao i original.
* `BDC` s imenovanim `/Properties` resursom (umjesto inline rječnika) Poppler ne javlja
  `OutputDev`-u, pa takav sadržaj ostaje bez MCID-a. Takav zapis je rijedak.

## Poppler 26.11 / FontForge 20251009

Kod se kompajlira i s Popplerom 24.06.1 (Debian bookworm) i s Popplerom 26.11.0 (Debian trixie).
Razlike API-ja rješava `src/util/poppler_compat.h`: tipovi argumenata virtualnih metoda izvode se
iz samog Popplera, a za ostalo služi `if constexpr` + `requires`. Sve nadjačane metode `OutputDev`-a
sada imaju `override`, pa kompajler hvata buduće promjene potpisa. Bez toga bi se npr. `drawString`
s Popplerom 26 tiho prestao pozivati (prazan HTML).

Važna promjena u Poppleru 26: pozicija teksta je u `getCurTextX/Y()`, a `getCurX/Y()` je samo
trenutna točka putanje.

Izmjereno na 18 testnih PDF-ova: vizualna vjernost s Popplerom 26.11 je jednaka ili malo bolja
(fontfile3), a semantički rezultati su identični.

## Mjerenja vjernosti: zaključci

* Širine redaka u HTML-u (mjereno u Chromeu) odgovaraju geometriji PDF-a unutar ±0.2 pt
  (prosjek +0.006 %). Raspored teksta je dakle točan.
* Preostalo odstupanje PDF-raster naspram HTML-rendera dolazi od rasterizacije glifova:
  Popplerovi rasterizeri Splash i Cairo međusobno se razlikuju za ~0.25 pt.
  `text-rendering: geometricPrecision` i veći `--font-size-multiplier` to ne mijenjaju.
* Postojeći bug u originalu: `CairoBackgroundRenderer::setMimeData` ne nadjačava Popplerovu metodu
  (drukčiji potpis), pa `--svg-embed-bitmap 0` ne izdvaja slike u zasebne datoteke.

## Razvoj

```bash
scripts/dev.sh image        # Docker image (Debian bookworm)
scripts/dev.sh deps         # Poppler 24.06.1 + FontForge 20230101 iz izvora (jednom)
scripts/dev.sh build        # inkrementalni build pdf2htmlEX-a
scripts/dev.sh test         # testovi semantičkog sloja
scripts/dev.sh run --preset semantic --dest-dir /work/out /work/dokument.pdf

# novije biblioteke: Debian trixie, Poppler 26.11.0, FontForge 20251009
VARIANT=trixie scripts/dev.sh image && VARIANT=trixie scripts/dev.sh deps && VARIANT=trixie scripts/dev.sh build
```

Provjere vjernosti (u containeru, uz baseline build `master` grane u `.baseline/`, vidi
`scripts/compare_outputs.sh`):

```bash
scripts/compare_outputs.sh OUT "" a.pdf b.pdf                 # bajt-usporedba, semantika isključena
NEW_OPTS="--semantic-tables 1" scripts/compare_outputs.sh OUT "" a.pdf
python3 scripts/visual_compare.py OUT/old/a.html OUT/new/a.html # pikselska usporedba
python3 scripts/fidelity_metric.py a.pdf OUT/new/a.html 4       # PDF naspram HTML-a, zum 4×
python3 scripts/semantic_dump.py OUT/new/a.semantic.json        # pregled tablica
```

Testni korpus je u `pdf2htmlEX/test/tables/`: izvori su u `src/` (LaTeX i HTML za LibreOffice),
očekivani rezultati u `expected/`. Novi slučaj: dodajte izvor, pokrenite `src/build.sh`, zatim
`P2H_TEST_GEN=1` test, pa **pregledajte** generirani JSON.

Kod: `pdf2htmlEX/src/Semantic/` (detekcija, izlaz), `src/HTMLRenderer/semantic.cc` (veza s
rendererom), `src/DrawingTracer.cc` (prikupljanje linija), `src/HTMLTextLine.cc` (anotacije).

Dijagnostika: `--debug 1` ispisuje linije i retke teksta stranice; varijabla okoline
`P2H_SEM_DEBUG=1` dodatno ispisuje korake detektora `hrules` (pojasevi, stupci, zaglavlje).
