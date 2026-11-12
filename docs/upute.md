# Upute za korištenje

Kako pretvoriti PDF u HTML s ovim buildom pdf2htmlEX-a, što se dobije, kako se rezultat koristi i
koje pomoćne naredbe postoje. Detalji o tome kako se prepoznaju tablice i odlomci su u
[semanticki-sloj.md](semanticki-sloj.md), a postupak provjere tablica na novom dokumentu u
[provjera-tablica.md](provjera-tablica.md).

Sadržaj:

1. [Ukratko](#1-ukratko)
2. [Prvo postavljanje](#2-prvo-postavljanje-jednom)
3. [Pretvorba PDF-a](#3-pretvorba-pdf-a)
4. [Što se dobije](#4-što-se-dobije)
5. [Opcije i recepti](#5-opcije-i-recepti)
6. [Rad u pregledniku](#6-rad-u-pregledniku)
7. [Strojna obrada](#7-strojna-obrada)
8. [Pomoćne naredbe](#8-pomoćne-naredbe-referenca)
9. [Česti problemi](#9-česti-problemi)

## 1. Ukratko

```bash
cd /home/mario/projects/pdftohtml/pdf2htmlEX
scripts/try_pdf.sh "/putanja/do/Izvještaj 2025.pdf" izvj
# rezultat: .realpdf/izvj/out/izvj.html, izvj.tables.html, izvj.semantic.json
```

* `izvj.html` izgleda isto kao PDF. Tipka **T** pokazuje prepoznate tablice, a Ctrl+C iz tablice
  daje jedan red tablice po retku u Excelu.
* `izvj.tables.html` sadrži samo tablice, kao obične HTML tablice.
* `izvj.semantic.json` sadrži sve tablice i odlomke za programe.

Sve se izvršava u Dockeru, pa na računalu trebaju samo `docker` i `bash`. Za provjeru tablica
izvan Dockera trebaju još `python3` i `pdftotext`.

## 2. Prvo postavljanje (jednom)

```bash
scripts/dev.sh image    # Docker image s alatima (nekoliko minuta)
scripts/dev.sh deps     # skine i kompajlira Poppler 24.06.1 i FontForge 20230101 (~10 min)
scripts/dev.sh build    # kompajlira pdf2htmlEX
scripts/dev.sh test     # opcionalno: testovi (svi moraju proći, "OK" na kraju)
```

Nakon svake promjene koda dovoljan je `scripts/dev.sh build` (inkrementalno, sekunde do minuta).
`image` i `deps` ponavljaju se samo kad se mijenja `docker/Dockerfile.dev` ili verzija biblioteka.

Sve što se kompajlira ostaje u mapi projekta i nije u gitu: `poppler-*/`, `fontforge-*/`,
`poppler-data/` i `pdf2htmlEX/build/`. Docker image se zove `pdf2htmlex-dev:bookworm`.

**Novije biblioteke** (Debian trixie, Poppler 26.11.0, FontForge 20251009) su druga varijanta s
odvojenim buildom u `pdf2htmlEX/build-trixie/`. Rezultati su isti; vidi
[semanticki-sloj.md](semanticki-sloj.md#poppler-2611--fontforge-20251009). Varijanta se bira
varijablom `VARIANT` ispred svake naredbe:

```bash
VARIANT=trixie scripts/dev.sh image
VARIANT=trixie scripts/dev.sh deps
VARIANT=trixie scripts/dev.sh build
VARIANT=trixie scripts/try_pdf.sh dokument.pdf ime
```

## 3. Pretvorba PDF-a

### 3.1 `scripts/try_pdf.sh`: preporučeno

```bash
scripts/try_pdf.sh PDF [IME] [opcije pdf2htmlEX-a...]
```

| Argument | Značenje |
|---|---|
| `PDF` | putanja do PDF-a bilo gdje na računalu. Ako ime sadrži razmake ili naša slova, stavite ga u navodnike |
| `IME` | kratko ime bez razmaka (npr. `izvjestaj`). Ako se izostavi, složi se iz imena datoteke |
| opcije | sve iza imena ide pdf2htmlEX-u, npr. `-f 10 -l 20` ili `--table-detect all` |

Skripta radi ovo:

1. kopira PDF u `.realpdf/IME/IME.pdf`. Mapa `.realpdf/` nije u gitu, a Docker vidi samo mapu
   projekta;
2. pretvori PDF s `--preset semantic` u `.realpdf/IME/out/`;
3. ispiše provjeru tablica (`check_tables.py check`, [§ 8.3](#83-scriptscheck_tablespy));
4. ako je isti `IME` već pretvaran, prethodni JSON sprema kao `.realpdf/IME/previous.semantic.json`
   i ispiše na kojim su se stranicama tablice promijenile.

Primjeri:

```bash
scripts/try_pdf.sh ~/Downloads/izvjestaj.pdf izvj                    # cijeli dokument
scripts/try_pdf.sh ~/Downloads/izvjestaj.pdf izvj -f 30 -l 35         # samo stranice 30–35
scripts/try_pdf.sh ~/Downloads/izvjestaj.pdf izvj --table-detect all  # i tablice bez linija
scripts/try_pdf.sh ~/Downloads/izvjestaj.pdf izvj --dehyphenate 1     # spoji rastavljene riječi
```

Pretvorba traje oko minute za 170 stranica.

### 3.2 `scripts/dev.sh run`: izravno

Za potpunu kontrolu nad opcijama i mjestom izlaza:

```bash
scripts/dev.sh run --preset semantic --dest-dir /work/.realpdf/rezultat /work/.realpdf/dokument.pdf
```

* Unutar Dockera projekt je na `/work`, pa sve putanje moraju počinjati s `/work/` i biti
  **unutar projekta**. PDF izvan projekta prvo kopirajte u projekt, npr. u `.realpdf/`.
* Putanja **ne smije imati razmake**: argumenti se prosljeđuju kroz `bash -c` pa bi se rastavili.
  Zato `try_pdf.sh` kopira PDF pod kratkim imenom.
* Iz istog razloga prazan argument (`""`) nestaje; umjesto njega uvijek dajte vrijednost
  (`--quiet 1`, ne `--quiet ""`).
* Bez `--dest-dir` izlaz ide u korijen projekta. Mapa `.realpdf/` nije u gitu, pa je dobro mjesto i
  za ulaz i za izlaz.

### 3.3 Docker image za druge programe (produkcija)

```bash
scripts/dev.sh build                      # binarij mora biti kompajliran
scripts/dev.sh runtime-image              # image pdf2htmlex:semantic-<git sha>
scripts/dev.sh runtime-image moj/tag:1.0  # ili s vlastitim imenom
```

Image (`docker/Dockerfile.runtime`) sadrži samo kompajlirani binarij, njegovu podatkovnu mapu i
nekoliko sistemskih biblioteka (Debian 12 slim, ~230 MB). Ulazna točka je `pdf2htmlEX`, kao u
službenom imageu, pa ga programi koji su koristili službeni image mogu koristiti bez promjena:

```bash
docker run --rm -v "$PWD:/pdf" -w /pdf pdf2htmlex:semantic-abbf97f \
    --semantic-tables 1 --semantic-output attrs,inline dokument.pdf
```

Naredba nakon builda pretvori `financial.pdf` iz testova i provjeri da izlaz ima `data-tbl`.

### 3.4 Bez Dockera

Binarna datoteka `pdf2htmlEX/build/pdf2htmlEX` kompajlirana je za Debian 12 i treba njegove
sistemske biblioteke. Na ovom računalu izravno pokretanje ne radi (nedostaju `libjpeg.so.62` i
`libxml2.so.2`), pa koristite Docker.

## 4. Što se dobije

S `--preset semantic`, za `dokument.pdf`:

| Datoteka | Za što |
|---|---|
| `dokument.html` | gledanje: stranice izgledaju kao PDF. Sadrži i nevidljivi semantički sloj ([§ 7.2](#72-html-atributi)) i skriptu za kopiranje i prikaz tablica |
| `dokument.tables.html` | samo tablice kao `<table>`, jedna za drugom, s naslovom stranice. Tablice koje se nastavljaju na sljedećoj stranici su spojene, a ponovljeno zaglavlje je izbačeno |
| `dokument.semantic.json` | sve tablice (ćelije, spojene ćelije, položaj) i odlomci, po stranicama ([§ 7.1](#71-semanticjson)) |

`dokument.html` je jedna samostalna datoteka: fontovi, slike, CSS i JS su ugrađeni. Može se
kopirati bilo kamo i otvoriti u pregledniku bez interneta.

Bez semantičkog sloja (`--preset fidelity` ili bez preseta) nastaje samo `dokument.html`, isti kao
iz originalnog pdf2htmlEX-a.

## 5. Opcije i recepti

### 5.1 Preseti

| Preset | Što uključuje |
|---|---|
| `--preset fidelity` | najvjerniji izgled: pozadina kao SVG (oštre linije i tekst grafike pri zumiranju), slike ugrađene u SVG, Type 3 fontovi kao tekst, ugrađivanje lokalnih fontova |
| `--preset semantic` | `fidelity` + `--semantic-tables 1` + svi izlazi (`--semantic-output attrs,inline,json,html`) |

Preset postavlja više opcija odjednom. Opcije navedene **iza** preseta ga pregazuju, npr.
`--preset semantic --bg-format png`.

### 5.2 Opcije semantičkog sloja

| Opcija | Zadano | Opis |
|---|---|---|
| `--semantic-tables 0\|1` | 0 | uključuje semantički sloj (preset `semantic` ga uključuje) |
| `--table-detect LISTA` | `tagged,ruled,hrules` | koji detektori tablica rade: `tagged` (iz tagova PDF-a), `ruled` (mreža linija), `hrules` (samo vodoravne linije), `stream` (bez linija, heuristika) ili `all` |
| `--semantic-output LISTA` | `attrs,inline,json` | `attrs`: `data-*` atributi na retcima teksta; `inline`: skriveni HTML po stranici; `json`: `.semantic.json`; `html`: `.tables.html` |
| `--semantic-paragraphs 0\|1` | 1 | grupira tekst izvan tablica u odlomke |
| `--dehyphenate 0\|1` | 0 | spaja riječi rastavljene na kraju retka (`elektro-` + `energetski`). U tagiranom PDF-u (InDesign) crtice koje je dodao program za prijelom prepoznaju se iz tagova i uklanjaju i bez ove opcije |
| `--cell-line-join space\|br` | space | retci teksta ćelije u HTML tablici: spojeni razmakom ili s `<br>` |
| `--semantic-spans 0\|1` | 0 | redak koji prelazi više ćelija omata se u `<span data-tbl…>` umjesto atributa `data-sem` |

### 5.3 Ostale korisne opcije pdf2htmlEX-a

| Opcija | Opis |
|---|---|
| `-f N`, `-l N` | prva i zadnja stranica |
| `--dest-dir MAPA` | mapa izlaza |
| `--zoom 1.5`, `--fit-width 1200` | veličina stranica u HTML-u |
| `--split-pages 1` | svaka stranica u zasebnoj datoteci |
| `--embed-font 0`, `--embed-image 0`, … | fontovi, slike, CSS i JS kao zasebne datoteke umjesto ugrađenih |
| `--quiet 1` | bez ispisa napretka |
| `--debug 1` | dijagnostički ispis, uključujući linije i retke teksta svake stranice |

Potpun popis: `scripts/dev.sh run --help`.

### 5.4 Recepti

| Želim… | Opcije |
|---|---|
| samo vjeran HTML za gledanje, bez ičega dodatnog | `--preset fidelity` |
| HTML za gledanje + tablice za Excel/obradu | `--preset semantic` |
| samo JSON za obradu, bez HTML tablica | `--semantic-tables 1 --semantic-output json` |
| tablice i bez ikakvih linija | `--preset semantic --table-detect all` |
| samo tablice iz tagova (PDF/UA, Word/LibreOffice „tagged PDF”) | `--preset semantic --table-detect tagged` |
| rastavljene riječi spojene i u netagiranom PDF-u | `--preset semantic --dehyphenate 1` |
| ćelije s prijelomima redaka kao u PDF-u | `--preset semantic --cell-line-join br` |
| manji HTML, PNG pozadina | `--preset semantic --bg-format png` |

## 6. Rad u pregledniku

Otvorite `dokument.html` (Chrome, Firefox, Edge).

| Radnja | Što se dogodi |
|---|---|
| tipka **T**, ili `#tables` na kraju adrese | okvir oko svake prepoznate tablice s oznakom (`p29t0 · 17×4` = stranica 29, tablica 0, 17 redova × 4 stupca) i obojena površina svake ćelije. Ponovni T ga sakrije |
| miš iznad ćelije, dok je prikaz uključen | ćelija dobije deblji okvir, a oznaka pokazuje red, stupac (od 1) i tekst ćelije |
| označiti dio tablice + Ctrl+C | u međuspremnik ide prava tablica (HTML) i TSV. U Excel/Calc/Word lijepi se jedan red PDF-a u jedan red, a višeredna ćelija ostaje jedna ćelija |
| označiti tekst + Ctrl+C | retci istog odlomka spajaju se razmakom umjesto prijelomom retka; rastavljene riječi i ligature (ﬁ, ﬂ) postaju normalne riječi |

`dokument.tables.html` je za brz pregled svih tablica dokumenta. Svaka tablica ima naslov s
oznakom, stranicom i detektorom (`p29t0 (page 29, hrules)`).

## 7. Strojna obrada

### 7.1 `.semantic.json`

```json
{
"version": 1,
"source": "/work/.realpdf/izvjestaj/izvjestaj.pdf",
"pages": [
{"page": 5, "width": 595.28, "height": 841.89,
 "tables": [
  {"id": "p5t0", "source": "tagged", "rows": 3, "cols": 2, "bbox": [79.8,132.9,319.5,240.1], "group": "p5t0",
   "cells": [
    {"r": 0, "c": 0, "cs": 2, "header": true, "bbox": [79.8,132.9,151.5,139.8],
     "text": "Indeks 2025./2024.", "lines": ["Indeks 2025./2024."]},
    {"r": 1, "c": 0, "bbox": [80.1,158.3,139.1,192.2], "text": "neto dobit 113,1", "lines": ["neto dobit", "113,1"]}
   ]}],
 "paragraphs": [
  {"id": "p5p1", "tag": "p", "bbox": [71.8,69.3,320.0,80.6], "text": "2. Najvažniji podaci za 2025. godinu"}
 ]}
]
}
```

| Polje | Značenje |
|---|---|
| `page`, `width`, `height` | broj stranice (od 1) i veličina u točkama (1/72 inča) |
| `bbox` | `[x0, y0, x1, y1]` u točkama, ishodište **gore lijevo** (kao `pdftotext -bbox`) |
| tablica `id` | `p<stranica>t<redni broj>`, isti kao `data-tbl` u HTML-u |
| `source` | detektor koji je našao tablicu: `tagged`, `ruled`, `hrules`, `stream` |
| `rows`, `cols` | veličina tablice |
| `group` | `id` prve tablice lanca; tablica koja se nastavlja preko više stranica ima isti `group` na svim stranicama |
| `continued_from` | samo na nastavku: `id` tablice s prethodne stranice |
| `repeated_header` | samo na nastavku: prvi red ponavlja zaglavlje (u `.tables.html` je izbačen) |
| ćelija `r`, `c` | red i stupac, **od 0** |
| `rs`, `cs` | broj redova/stupaca spojene ćelije (izostavljeno kad je 1) |
| `header` | ćelija zaglavlja (TH iz tagiranog PDF-a) |
| `text` | tekst ćelije, retci spojeni razmakom (rastavljene riječi spojene, ligature raspisane) |
| `lines` | retci teksta ćelije kako su u PDF-u |
| odlomak `tag` | `p`, `li`, `caption` ili `h1`–`h6` (naslovi i natuknice samo iz tagiranog PDF-a) |

Prazna ćelija može nedostajati ili imati prazan `text`; mjesta koja nisu navedena su prazna. Redoslijed odlomaka je redoslijed čitanja: višestupčani tekst stupac
po stupac, a u tagiranom PDF-u redoslijed iz tagova.

Primjer: sve tablice dokumenta u CSV datoteke, s nastavcima spojenima u jednu tablicu:

```python
import csv, json, sys

doc = json.load(open(sys.argv[1], encoding='utf-8'))
groups = {}                                   # group -> list of rows
for page in doc['pages']:
    for t in page['tables']:
        grid = [[''] * t['cols'] for _ in range(t['rows'])]
        for c in t['cells']:
            grid[c['r']][c['c']] = c['text']
        if t.get('repeated_header'):
            grid = grid[1:]
        groups.setdefault(t['group'], []).extend(grid)

for gid, rows in groups.items():
    with open(f'{gid}.csv', 'w', newline='', encoding='utf-8-sig') as f:   # utf-8-sig: Excel prepozna naša slova
        csv.writer(f, delimiter=';').writerows(rows)
```

```bash
python3 tablice_u_csv.py .realpdf/izvj/out/izvj.semantic.json   # p5t0.csv, p29t0.csv, ...
```

Tekst spojene ćelije (`rs`/`cs`) je samo u njezinom gornjem lijevom polju.

### 7.2 HTML atributi

U `dokument.html` svaki redak teksta (`<div class="t">`) nosi oznaku čemu pripada:

| Atribut | Značenje |
|---|---|
| `data-tbl="p1t0" data-r="2" data-c="1"` | cijeli redak je u ćeliji (red 2, stupac 1, od 0) |
| `data-sem="t:p1t0:4:2@0-4 t:p1t0:4:3@5-21"` | redak prelazi više ćelija: znakovi 0–4 su u ćeliji (4,2), 5–21 u (4,3). Pomaci su UTF-16, kao `String` u JavaScriptu |
| `data-para="p1p3"` | redak pripada odlomku (`id` iz JSON-a) |
| `data-tag="h1"` | naslov ili drugi element iz tagova |
| `data-hy="1"` | redak završava crticom koju je dodao program za prijelom |

Svaka stranica ima i skriveni `<div class="pdf2htmlEX-semantic" data-page-no="N" hidden>` u kojem
su njezine tablice (`<table data-tbl=… data-source=…>`) i odlomci (`<p>`, `<h1>`, …) redoslijedom
čitanja. Taj sloj služi za dvije stvari:

* **kopiranje**: skripta iz vidljivih redaka zna koje su ćelije označene, a iz skrivene tablice
  uzima njihov puni tekst (i kad je označen samo dio prelomljene ćelije) i spojene ćelije. Bez
  njega (`--semantic-output` bez `inline`) kopiranje i dalje daje redove tablice, ali samo od
  označenih komadića teksta i bez spojenih ćelija;
* **programe koji čitaju HTML**: tablice i odlomci dostupni su iz samog `dokument.html`, bez
  `.semantic.json`.

Čitači ekrana ga ne čitaju: atribut `hidden` uklanja element i iz onoga što vide čitači ekrana.
Oni čitaju vidljive retke teksta, bez strukture tablice.

Primjer čitanja iz HTML-a:

```js
// u konzoli preglednika: sve tablice kao nizovi redova
[...document.querySelectorAll('.pdf2htmlEX-semantic table')].map(t =>
  [...t.rows].map(r => [...r.cells].map(c => c.textContent)))
```

## 8. Pomoćne naredbe (referenca)

Sve su u `scripts/`. Python skripte se mogu pokrenuti u Dockeru preko
`scripts/dev.sh exec python3 scripts/…`, s putanjama koje počinju s `/work/`. `check_tables.py` i
`semantic_dump.py` rade i na računalu.

### 8.1 `scripts/dev.sh`

| Naredba | Što radi |
|---|---|
| `image` | izgradi Docker image (`docker/Dockerfile.dev`) |
| `deps` | skine i kompajlira Poppler, FontForge i poppler-data u mapu projekta. Već kompajlirano preskače |
| `build` | kompajlira pdf2htmlEX i instalira podatkovne datoteke u `pdf2htmlEX/build/install/` |
| `run OPCIJE… PDF` | pokrene upravo kompajlirani pdf2htmlEX ([§ 3.2](#32-scriptsdevsh-run-izravno)) |
| `test` | testovi semantičkog sloja (`pdf2htmlEX/test/tables/test_tables.py`) |
| `shell` | interaktivni shell u kontejneru, u `/work` |
| `exec NAREDBA…` | jedna naredba u kontejneru, npr. `scripts/dev.sh exec python3 scripts/check_tables.py …` |
| `runtime-image [TAG]` | Docker image s kompajliranim binarijem za druge programe ([§ 3.3](#33-docker-image-za-druge-programe-produkcija)) |

| Varijabla okoline | Značenje |
|---|---|
| `VARIANT=bookworm\|trixie` | izbor biblioteka ([§ 2](#2-prvo-postavljanje-jednom)); zadano `bookworm` |
| `PDF2HTMLEX_DEV_IMAGE` | drugo ime Docker imagea |
| `SOURCE_DATE_EPOCH` | vrijeme upisano u fontove. Fiksno je (1700000000) da dva pokretanja daju bajt-identičan HTML |

Varijable za sam pdf2htmlEX predaju se kroz `exec`:

```bash
scripts/dev.sh exec "P2H_SEM_DEBUG=1 pdf2htmlEX/build/pdf2htmlEX --data-dir pdf2htmlEX/build/install/usr/local/share/pdf2htmlEX --preset semantic -f 29 -l 29 --dest-dir /work/.realpdf/izvj/debug /work/.realpdf/izvj/izvj.pdf"
```

### 8.2 `scripts/try_pdf.sh`

Pretvorba + provjera + usporedba s prethodnim pokretanjem; vidi [§ 3.1](#31-scriptstry_pdfsh-preporučeno).

### 8.3 `scripts/check_tables.py`

Uspoređuje tablice iz `.semantic.json` s tekstom PDF-a i javlja što treba pogledati. Detaljno
objašnjenje oznaka i postupka je u [provjera-tablica.md](provjera-tablica.md).

```bash
python3 scripts/check_tables.py check JSON PDF [--pages 3,5-7] [--all]
python3 scripts/check_tables.py show  JSON [--pages 29]
python3 scripts/check_tables.py diff  STARI.json NOVI.json [--show]
```

| Podnaredba | Što radi |
|---|---|
| `check` | za svaku tablicu traži brojeve u njenom području koji nisu ni u jednoj ćeliji (`MISSING`), brojeve iz istog tiskanog retka u različitim redovima (`SPLIT`) i dugi tekst u ćelijama (`TEXT?`, vjerojatno lažna tablica). Za svaku stranicu traži i retke s više razmaknutih brojeva izvan tablica (`OUTSIDE`, moguće promašena tablica). `--all` ispisuje i tablice bez primjedbi |
| `show` | ispisuje tablice red po red: `[stupac] tekst`, uz `cs=`/`rs=` za spojene ćelije |
| `diff` | popis stranica na kojima se tablice razlikuju između dva JSON-a (regresije nakon promjene koda); `--show` ispisuje i same razlike |

Treba `pdftotext` (paket `poppler-utils`) samo za `check`.

### 8.4 `scripts/semantic_dump.py`

```bash
python3 scripts/semantic_dump.py dokument.semantic.json [drugi.semantic.json …]
```

Ispisuje sve tablice ćeliju po ćeliju (`r2 c1 cs=2 H: 'tekst'`, H = zaglavlje), zatim sve odlomke
s oznakom (`p`, `h1`, …) i početkom teksta. Koristan za pregled odlomaka i redoslijeda čitanja;
za tablice je pregledniji `check_tables.py show`.

### 8.5 Provjere vjernosti (za razvoj)

Ove skripte uspoređuju rezultat s originalnim pdf2htmlEX-om ili s PDF-om. Pokreću se u
kontejneru.

**Baseline.** `compare_outputs.sh` uspoređuje s buildom grane `master` u mapi `.baseline/` (git
worktree, kompajliran isto kao glavni build). Na ovom računalu već postoji. Iznova:
`git worktree add .baseline master`, pa ga kompajlirati kao u `scripts/dev.sh build` (u
`.baseline/pdf2htmlEX/build/`).

| Naredba | Što radi |
|---|---|
| `scripts/compare_outputs.sh IZLAZ "OPCIJE" a.pdf b.pdf…` | pretvori svaki PDF baseline buildom (u `IZLAZ/old/`) i trenutnim buildom (u `IZLAZ/new/`), s istim opcijama, i za svaki ispiše `IDENTICAL` ili `DIFFERENT` (bajt-usporedba HTML-a). Opcije samo za novi build idu u varijablu `NEW_OPTS` |
| `python3 scripts/visual_compare.py STARI.html NOVI.html […]` | renderira oba HTML-a u Chromiumu i uspoređuje piksele. Izlazni kod 1 ako se razlikuju |
| `python3 scripts/fidelity_metric.py a.pdf a.html [ZUM]` | prva stranica PDF-a (pdftoppm) naspram HTML-a (Chromium): prosječna razlika (0–255) i udio jako različitih piksela. Manje je bolje; 0 nije dostižna zbog razlika u antialiasingu |

Primjer: dokaz da semantički sloj ne mijenja izgled:

```bash
# bez semantike: mora biti IDENTICAL
scripts/dev.sh exec scripts/compare_outputs.sh /work/.realpdf/cmp --quiet=1 /work/.realpdf/izvj/izvj.pdf
# sa semantikom: HTML je DIFFERENT (dodani atributi i skriveni sloj), a render mora biti PIXEL-IDENTICAL
scripts/dev.sh exec 'NEW_OPTS="--semantic-tables 1" scripts/compare_outputs.sh /work/.realpdf/cmp --quiet=1 /work/.realpdf/izvj/izvj.pdf'
scripts/dev.sh exec python3 scripts/visual_compare.py /work/.realpdf/cmp/old/izvj.html /work/.realpdf/cmp/new/izvj.html
```

`compare_outputs.sh` mora dobiti drugi argument (opcije), a kroz `dev.sh exec` prazan `""`
nestane. Zato se ovdje daje bezopasna opcija `--quiet=1`.

### 8.6 Testovi

```bash
scripts/dev.sh test                                                         # svi testovi
scripts/dev.sh exec python3 pdf2htmlEX/test/tables/test_tables.py TableTests.test_word_single_number_column
scripts/dev.sh exec "P2H_TEST_GEN=1 python3 pdf2htmlEX/test/tables/test_tables.py"   # ponovno složi očekivanja
```

* Testni PDF-ovi nastaju iz izvora u `pdf2htmlEX/test/tables/src/` (LaTeX `.tex`, HTML za
  LibreOffice tagirani PDF): `scripts/dev.sh exec bash pdf2htmlEX/test/tables/src/build.sh`.
* Očekivani rezultati su u `pdf2htmlEX/test/tables/expected/IME.json`. Nakon `P2H_TEST_GEN=1`
  pregledajte promjene (`git diff`) prije commita: test tada samo zapiše ono što program trenutno
  daje.
* Testovi rade i s `VARIANT=trixie`.

### 8.7 Dijagnostičke varijable okoline

| Varijabla | Što ispisuje |
|---|---|
| `P2H_SEM_DEBUG=1` | korake detektora `hrules`: skupine linija, pojaseve, stupce, retke iznad i ispod linija, „fallback” |
| `P2H_PARA_DEBUG=1` | za odlomke: pronađene razmake između stupaca teksta i dijelove redaka (`para: seg …`) iz kojih se slažu odlomci i redoslijed čitanja |
| `--debug 1` (opcija) | linije (ravnala) i retke teksta svake stranice |

## 9. Česti problemi

| Problem | Uzrok i rješenje |
|---|---|
| `Unable to find image 'pdf2htmlex-dev:bookworm'` | nije izgrađen image: `scripts/dev.sh image` (i `deps`, `build`) |
| `pdf2htmlEX/build/pdf2htmlEX: No such file or directory` | nije kompajlirano: `scripts/dev.sh build` |
| `Couldn't open file …` / `Error: Cannot read the file` s `dev.sh run` | PDF nije unutar projekta, putanja ne počinje s `/work/` ili ima razmak. Koristite `try_pdf.sh` ili kopirajte PDF u `.realpdf/` |
| datoteke izlaza pripadaju root-u | ne bi trebalo: kontejner radi s vašim UID-om. Ako ste ga pokrenuli s `sudo`, popravite s `sudo chown -R $USER .realpdf` |
| tablica nije prepoznata | nema linija: probajte `--table-detect all`. Inače `check_tables.py show` i `P2H_SEM_DEBUG=1` za tu stranicu ([provjera-tablica.md](provjera-tablica.md)) |
| tekst ima `` ili čudne znakove u JSON-u | PDF za taj font nema podatak o Unicode znaku (najčešće Type 3 bitmap fontovi); znak se ne može vratiti ni u Acrobatu |
| promjena koda nema učinka | zaboravljen `scripts/dev.sh build`, ili se koristi druga varijanta (`VARIANT`) od one koja je kompajlirana |
| HTML je velik | SVG pozadina je veća od PNG-a: `--bg-format png` (malo manje oštro pri zumiranju) |
