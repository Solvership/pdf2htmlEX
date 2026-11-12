# Isprobavanje novog PDF-a i provjera tablica

Upute kako pretvoriti bilo koji PDF sa semantičkim slojem i provjeriti jesu li tablice
prepoznate ispravno. Što semantički sloj jest i koje opcije postoje opisano je u
[semanticki-sloj.md](semanticki-sloj.md), a postavljanje i sve naredbe u [upute.md](upute.md).

## Brzo: jedna naredba

```bash
cd /home/mario/projects/pdftohtml/pdf2htmlEX
scripts/try_pdf.sh "/putanja/do/Neki izvještaj 2025.pdf" izvj
```

* prvi argument je PDF (smije imati razmake i naša slova u imenu, navodnici su obavezni);
* drugi argument je kratko ime bez razmaka (npr. `izvj`); ako ga izostavite, složi se iz imena
  datoteke;
* iza imena mogu doći dodatne opcije pdf2htmlEX-a, npr. `-f 10 -l 20` (samo stranice 10–20) ili
  `--table-detect all` (i tablice bez linija).

Skripta:

1. kopira PDF u `.realpdf/IME/IME.pdf` (mapa `.realpdf/` nije u gitu; Docker vidi samo mapu
   projekta);
2. pretvori ga s `--preset semantic` u `.realpdf/IME/out/` (`IME.html`, `IME.tables.html`,
   `IME.semantic.json`);
3. pokrene provjeru tablica (`check`, dolje);
4. ako je PDF već bio pretvoren, usporedi novi rezultat s prethodnim (`diff`, dolje) — korisno nakon
   promjene detektora.

Na kraju ispiše gdje je rezultat. Pretvorba velikog dokumenta (170 stranica) traje oko minute.

Ako je kod u međuvremenu mijenjan, prvo `scripts/dev.sh build`.

## Pregled u pregledniku

* `IME.html` — stranice izgledaju kao PDF. Tipka **T** (ili `#tables` na kraju adrese) iscrta
  okvir svake prepoznate tablice (`p29t0 · 17×4` = stranica 29, tablica 0, 17 redova × 4 stupca) i
  svake ćelije; prelazak mišem pokaže red, stupac i tekst ćelije. Što nema okvir, nije prepoznato
  kao tablica.
* `IME.tables.html` — samo tablice, kao obične HTML tablice.
* Najbrža praktična provjera: označite tablicu u `IME.html`, Ctrl+C, zalijepite u Excel — broj
  redova i stupaca mora odgovarati PDF-u.

## `scripts/check_tables.py`

Radi na računalu (treba `python3` i `pdftotext` iz paketa poppler-utils) ili u kontejneru
(`scripts/dev.sh exec python3 scripts/check_tables.py ...`, putanje tada počinju s `/work/`).

### `check` — usporedba s tekstom PDF-a

```bash
python3 scripts/check_tables.py check .realpdf/izvj/out/izvj.semantic.json .realpdf/izvj/izvj.pdf
python3 scripts/check_tables.py check ... --pages 29-32   # samo te stranice
python3 scripts/check_tables.py check ... --all           # i tablice bez primjedbi
```

Za svaku tablicu uspoređuje riječi PDF-a (`pdftotext -bbox`) unutar područja tablice s
tekstom ćelija. Ispisuje samo ono što treba pogledati:

| Oznaka | Značenje | Što obično znači |
|---|---|---|
| `MISSING 123 …` | broj je unutar područja tablice, a nije ni u jednoj ćeliji | izgubljen red ili dio reda, krivo prepoznat rub tablice |
| `SPLIT a b \| …` | brojevi iz istog tiskanog retka završili su u različitim redovima tablice | red je razdvojen na dva |
| `TEXT? …` | tablica iz linija ima ćelije s dugim tekstom | vjerojatno lažna tablica (stranica teksta, grafikon) |
| `OUTSIDE …` | redak s više brojeva razmaknutih kao stupci nije ni u jednoj tablici | promašena tablica — ili tablica **bez ikakvih linija** (isključene su dok ih ne uključite s `--table-detect all`), grafikon, sadržaj |

Na kraju je sažetak: broj tablica po izvoru (`tagged` = iz tagova PDF-a, `ruled` = puna mreža
linija, `hrules` = samo vodoravne linije, `stream` = bez linija), broj tablica s primjedbama i broj
stranica s brojevima izvan tablica. Zaglavlje i podnožje stranice (gornjih i donjih 7 %) se ne
gledaju, da brojevi stranica ne stvaraju šum.

Provjera je pomoć, ne presuda:

* `OUTSIDE` javlja i grafikone (oznake osi, postoci), sadržaj dokumenta i infografike — to nisu
  tablice;
* broj koji je u ćeliji, ali u krivom stupcu, provjera ne vidi — za to služi `show` ili prikaz
  tipkom T;
* tablica koja izgleda dobro, a krivo je spojena s tekstom ispod/iznad, vidi se u `show`.

### `show` — tablice red po red

```bash
python3 scripts/check_tables.py show .realpdf/izvj/out/izvj.semantic.json --pages 29
```

```
p29 p29t0 hrules 17x4
     0  [1] Bilješka  |  [2] 2025.  |  [3] 2024.
     1  [0] Prihodi iz poslovanja  |  [1] 4  |  [2] 27.879  |  [3] 36.160
     ...
```

`[2]` je stupac (od 0), `cs=2` / `rs=2` ćelija preko 2 stupca / reda. Usporedite s PDF-om (ili s
`pdftotext -layout -f 29 -l 29 dokument.pdf -`).

### `diff` — što se promijenilo između dva pokretanja

```bash
python3 scripts/check_tables.py diff stari.semantic.json novi.semantic.json          # popis stranica
python3 scripts/check_tables.py diff stari.semantic.json novi.semantic.json --show   # i razlike
```

Za regresije nakon promjene detektora: spremite `IME.semantic.json` prije promjene (`try_pdf.sh` to
radi sam, kao `.realpdf/IME/previous.semantic.json`), pretvorite ponovno i pogledajte da su se
promijenile samo stranice koje ste htjeli popraviti.

## Kako javiti problem

Najkorisnije je: ime dokumenta (kratko ime iz `try_pdf.sh`), broj stranice i što je krivo
(npr. „str. 52, druga tablica nije prepoznata” ili „str. 29, red X razdvojen na dva”), uz izlaz
naredbe `check` za tu stranicu.

## Postupak koji se pokazao pri razvoju

Na dva stvarna dokumenta (godišnji izvještaj iz InDesigna, 170 str.; izvještaj iz Worda, 81 str.)
provjera je išla ovako:

1. `try_pdf.sh` → pregled izlaza `check`;
2. za svaku primjedbu `show` za tu stranicu i usporedba sa slikom stranice
   (`pdftoppm -f N -l N -r 70 -png dokument.pdf slika`);
3. uzrok u detektoru: `P2H_SEM_DEBUG=1` ispisuje korake detektora linija (grupe linija, stupci,
   retci gore/dolje, „fallback”);
4. nakon popravka: `diff` na **svim** dosadašnjim dokumentima (promijeniti se smiju samo ciljane
   stranice) i `scripts/dev.sh test`.

Prvi prolaz na novom izvoru dokumenta obično otkrije nove slučajeve: na Word dokumentu prva je
verzija imala ~20 lažnih tablica i promašila glavne izvještaje, dok je na InDesign dokumentu
radila dobro.
