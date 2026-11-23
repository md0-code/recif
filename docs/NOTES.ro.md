# Note

Modelele CORAL, pachetele de discuri, 2.11BSD, terminalul, legăturile seriale, imaginile de bandă și compatibilitatea. Tastele sunt în [README](../README.ro.md).

## Modelele CORAL

Panoul calculatorului arată echipamentul descris de sursele istorice și
configurația de referință DEC care rulează programele. Coloana de memorie
de mai jos dă memoria RAM implicită a acestei configurații de referință.

| Model CORAL | Echipament documentat | Referință de execuție și RAM implicit |
| --- | --- | --- |
| 4001 | 64 KB raportați și circuite bit-slice AMD 2901. Sursele descriu primul model de 16 biți și compatibilitatea RT-11. [Raportul de lansare din 1979](https://www.bibliotecadeva.ro/periodice/scanteia/1979/06/scanteia_1979_06_11454.pdf) | Subsetul întreg de bază PDP-11/04, 56 Kocteți |
| 4011 | 256 KB raportați și o comparație contemporană cu PDP-11/34. [Raportul de lansare din 1979](https://www.bibliotecadeva.ro/periodice/scanteia/1979/06/scanteia_1979_06_11454.pdf), [catalogul de calculatoare din 1984](https://retropages.hu/Download/A%20szocialista%20orszagok%20mikro_%20es%20miniszam%C3%ADtogepei.pdf) | Subsetul întreg PDP-11/34 cu MMU unificat pe 18 biți, 248 Kocteți |
| 4021 | 2 MB în schema de dispecer și 4096 KB în studiul contemporan. Studiul raportează un procesor de virgulă mobilă. Schema de dispecer enumeră disc fix de 40 MB, dischete de 0,5 MB, bandă magnetică, MPX 40 A/B, ALC 41 și I/O teritorial DEN 40. [Schema de dispecer, anexa D](https://era.ed.ac.uk/items/3fdbc5a2-088b-46af-9e29-e2d5b4d1ac92), [studiul tehnic din 1984](https://www.kshkonyvtar.hu/file-show/L2hvbWUvc2lsaW5mby9hcHBsaWNhdGlvbi93b3JrZGlyL2NrZWRpdG9yQ3VzdG9tRmlsZXMvRsOpbnllcyBFbGVrIGRpZ2l0w6FsaXMgc3RhdGlzenRpa2FpIGvDtm55dnTDoXIvU3rDoW3DrXTDoXN0ZWNobmlrYSAoZm9secOzaXJhdCkvMTk4NC9JSUlfQV8xNDYwX1N6YW1pdGFzdGVjaG5pa2FfODRfMDNfdmoucGRm) | Experiment PDP-11/44 cu memorie extinsă, 2048 Kocteți |
| 4030 | O magistrală modulară asincronă și interfața QB-40 spre QBUS. Sursele vremii raportează până la 4 MB, opțiuni de disc, bandă, imprimantă și cititor de cartele și compatibilitate cu programele existente pentru 11/44. [Raportul CORAL 4030 din 1983](https://www.bibliotecadeva.ro/periodice/revistaeco/1983/08/revista_economica_1983_08_31.pdf), [studiul tehnic din 1984](https://www.kshkonyvtar.hu/file-show/L2hvbWUvc2lsaW5mby9hcHBsaWNhdGlvbi93b3JrZGlyL2NrZWRpdG9yQ3VzdG9tRmlsZXMvRsOpbnllcyBFbGVrIGRpZ2l0w6FsaXMgc3RhdGlzenRpa2FpIGvDtm55dnTDoXIvU3rDoW3DrXTDoXN0ZWNobmlrYSAoZm9secOzaXJhdCkvMTk4NC9JSUlfQV8xNDYwX1N6YW1pdGFzdGVjaG5pa2FfODRfMDNfdmoucGRm) | Subsetul întreg PDP-11/44 cu MMU cu spații I/D separate, 4088 Kocteți |

Limita de adresare a referinței include o pagină de I/O de 8 Kocteți.
Panoul arată memoria RAM utilizabilă lângă cantitatea aleasă. Dispozitivele
de referință DEC sunt o interfață serială DL11, un ceas KW11-L, o bandă
TM11 numai pentru citire cu o unitate și un cuplor de disc: un RH11 cu două
unități RM03 sau RP06, sau un RP11-C cu două unități RP03. Ceasul merge la
50 Hz, dacă nu se alege 60 Hz sau nu se oprește; generarea de sistem RSX
recuperată de pe o bandă CORAL este făcută pentru rețeaua de 50 Hz. Banda
TM11 este pornită pentru modelele cu bandă documentată. Modelul 4021
pornește cu RP11-C, al cărui disc RP03 de 40 MB corespunde discului din
instalația 4021 documentată, iar 4030 cu RH11 și unități RM03. Acestea se
schimbă în panou sau, din linia de comandă, cu `--tape`, `--disk` și
`--disks`.

Procesorul de virgulă mobilă FP11 urmează descrierea DEC: formatele F și
D, modurile FPS și capcana de excepție prin 244. Este pornit pentru 4021,
la care procesorul de virgulă mobilă este raportat, și se poate porni
pentru 4011 și 4030 cu `--fpu` sau din panou. Referința pentru 4001 nu are
opțiunea FP11.
[Manualul procesoarelor DEC](https://bitsavers.trailing-edge.com/pdf/dec/pdp11/handbooks/EB-19402-20_PDP-11_Processor_Handbook_1981.pdf)

Microcodul procesorului CORAL, memoriile ROM de încărcare, adresele
registrelor cuploarelor și interfața procesorului de virgulă mobilă al
modelului 4021 lipsesc din documentele disponibile. FP11, RH11 și RP11-C
sunt înlocuitori DEC ai acestora. Plăcile de memorie externă și de
interfață din tabelul de mai sus sunt înregistrări de echipament; emularea
lor cere specificațiile lor de programare. 4015, variantele cu sufix și
8730 din familia VAX sunt modele separate.

Un detaliu al modelului 4030 provine din programe. Sistemul RSX-11M V4.1 de
pe banda RSX41FP mută MMR3 la 165410 („corecție pt. CORAL 4030”) și verifică
existența hărții UNIBUS și a cuploarelor de disc pe 22 de biți. De aceea
referința 4030 răspunde la MMR3 atât la 172516, cât și la 165410, nu are
hartă UNIBUS, ca pe o magistrală QBUS, iar RH11 are registrul de extensie a
adresei RH70 la 176750.

Profilurile se văd cu `recif.exe models`, iar sintaxa comenzilor cu
`recif.exe --help`.

## Pachete de discuri

Fiecare unitate folosește o imagine plată de sectoare de 512 octeți, în
ordinea cilindru, pistă, sector.

| Unitate | Cuplor | Geometrie | Mărimea imaginii |
| --- | --- | --- | --- |
| RP03 | RP11-C la 176710 | 406 cilindri, 20 piste, 10 sectoare | 41.574.400 octeți |
| RM03 | RH11 la 176700 | 823 cilindri, 5 piste, 32 sectoare | 67.420.160 octeți |
| RP06 | RH11 la 176700 | 815 cilindri, 19 piste, 22 sectoare | 174.423.040 octeți |

Ambele cuploare întrerup prin vectorul 254, de aceea un calculator are unul
singur. Un nume de fișier nou ales cu **Alegere** creează un pachet gol.
Spațiul nescris încă se citește ca zerouri. Un fișier care nu se poate
deschide pentru scriere se montează protejat la scriere.

Sursele de încărcare **Discul unității** citesc primele 512 cuvinte ale
pachetului la adresa zero și le pornesc cu numărul unității în R0 și
registrul de comandă al cuplorului în R1 (176700 pentru RH11, 176714 pentru
RP11-C), ca încărcătoarele DEC.

`recif.exe bru-restore` reface discul sursă al unei benzi de salvare BRU ca
imagine de pachet. Fiecare bloc revine la locul său inițial, astfel încât un
sistem RSX salvat își regăsește taskurile instalate; fișierul index, harta
de alocare și cataloagele sunt refăcute. Seturile de salvare ulterioare ale
aceluiași volum de pe bandă sunt adăugate. Banda RSX41FP se restaurează ca
sistem RSX-11M V4.1 care pornește de pe un RP06 la CORAL 4030:

```powershell
.\recif.exe bru-restore .\RSX41FP.tap .\rsx41fp.dsk --disk rp06
.\recif.exe run --model 4030 --disks rp06 --disk .\rsx41fp.dsk --boot-disk 0 --serial tcp-listen:2323
```

La cererea de dată se răspunde cu Enter sau cu ora și data, de exemplu
`12:00 22-NOV-86`. Banda conține numai primele cataloage ale discului, de
aceea fișierul de pornire semnalează unele fișiere lipsă. Pachetele
restaurate de pe celelalte benzi se montează ca discuri de date pe unitatea
1, de exemplu `MOU DB1:CTCI102` și `RUN DB1:[377,15]ANIMAL` pentru banda
cu jocuri CORAL4.

## 2.11BSD

`disks\unix211-rp06.dsk` conține 2.11BSD (aprilie 2000), UNIX-ul de la
Berkeley pentru PDP-11, instalat de pe banda sa originală de distribuție:
rădăcina, zona de swap și un `/usr` cu compilatoare, jocuri și sursele
complete. Rulează pe profilul CORAL 4021, a cărui referință 11/44 are harta
UNIBUS de care 2.11BSD are nevoie.

O distribuție proaspăt despachetată pornește pregătită pentru el: CORAL
4021, **RH11 cu RP06**, pachetul pe unitatea 0 ca sursă de încărcare și un
DAF-2020 în modul VT100 care se leagă la `127.0.0.1:2323` cu comutatoarele
S4 și S5 pe ON și S7 pe OFF, deoarece consola BSD trimite caractere de 7 biți
cu paritate pară. Căile relative ale imaginilor din panou numesc fișiere de
lângă `recif.exe`.
Setările salvate anterior au întâietate; ștergerea `recif.yaml` și
`daf2020.yaml` revine la această configurare.

1. Se pornesc `recif.exe` și `daf2020.exe`, apoi se apasă **Pornire**.
2. La promptul `:` se apasă Enter. La promptul `#` (un singur utilizator) se
   apasă Ctrl+D, apoi se intră ca `root` (fără parolă).
3. Înainte de oprirea Recif se dă `shutdown -h now`.

```powershell
.\recif.exe run --model 4021 --disks rp06 --disk .\disks\unix211-rp06.dsk --boot-disk 0 --serial tcp-listen:2323
```

**Viteza maximă a gazdei** din panou, sau `--full-speed`, rulează
procesorul cât de repede permite calculatorul cât timp sistemul lucrează și
în ritmul unui 11/44 cât timp așteaptă; ceasul sistemului o ia atunci
înainte. În arborele de surse `python tools\unix211.py` reface pachetul de
pe bandă. [Licențe](THIRD_PARTY.md#211bsd)

## Terminalul DAF-2020

Terminalul pornește în modul de lucru Tektronix 4010. Ca în manual, afișează
literele mari până primește SO, iar tastatura lui trimite numai litere mari.
`ESC X 1` sau **CTRL+PF1** urmat de **1** trece în modul VT100, iar `ESC 1`
revine în modul Tektronix. În modul Tektronix, tastele săgeți mută fereastra
de vizualizare de 288 de linii peste spațiul de lucru de 390 de linii;
**SCRL+SHIFT** fixează fereastra, iar **SCRL+CTRL** o face din nou să urmeze
cursorul. În modul VT100, **SCRL+CTRL** deschide SET-UP A, iar tasta **5** a
acestuia trece în SET-UP B. Ce se schimbă acolo rămâne până la resetarea
sau oprirea terminalului.

| Tasta DAF-2020 | Tasta PC |
| --- | --- |
| PF1–PF4 | F1–F4 |
| PAGE | F5 |
| RESET | F6 |
| LF | F7 |
| BREAK | F8 sau Pause |
| SCRL | F9 sau Scroll Lock |
| Resetare cu ștergerea ecranului (ca ESC c) | Alt+F12 |
| Oprire și pornire | Ctrl+F12 sau **Repornire** din F12 |
| Ieșire | Shift+F12 |

**F12** deschide setările: limba interfeței, modul de lucru Tektronix sau
VT100, legătura și adresa ei, cele 14 comutatoare ale tastaturii, efectul
CRT, ecranul normal sau inversat și repornirea. Săgețile sus și jos aleg un
rând, stânga și dreapta aleg un comutator, iar **Enter** deschide valorile
unui rând, comută un comutator sau editează adresa. **Esc** sau **F12**
închide setările și le aplică; limba, efectul CRT și ecranul se aplică
imediat. Ecranul arată un tub cu fosfor verde, iar inversat caractere
întunecate pe tubul luminos.

Comutatoarele sunt numerotate ca în manual. ON este poziția de sus.

| Comutator | Efect |
| --- | --- |
| S1–S3 | Viteză de 150 × 2ⁿ bauds: S1 ON adaugă 4 la n, S2 ON adaugă 2, S3 ON adaugă 1 |
| S4 | ON paritate pară, OFF paritate impară |
| S5 | ON cu paritate, OFF fără paritate |
| S6 | ON doi biți de STOP |
| S7 | ON caracter de 8 biți, OFF de 7 biți |
| S8 | ON cuplat la calculator, OFF local |
| S9, S10 | Răspunsurile GIN se termină cu CR când S10 este ON și cu CR EOT când și S9 este ON |
| S11 | ON ecou local |
| S12 | ON LF face și CR |
| S13 | ON BREAK la pagină plină |
| S14 | ON click pe tastă |

## Legături seriale

Unitatea centrală își alege legătura DL11 și formatul în panoul ei.
DAF-2020 își alege legătura în setările **F12**: client sau server TCP, un
pipe, un port serial al calculatorului sau o buclă. Formatul ei urmează
comutatoarele și SET-UP B. Când unitatea centrală folosește TCP, un pipe sau
un port serial, așteaptă prima conexiune înainte de a porni programul.
Butonul **Repornire** repornește unitatea centrală. **Ctrl+F12** oprește și
pornește DAF-2020. În terminal, **Alt+Enter** comută ecranul complet, iar
**Print Screen** salvează un PNG.

| Legătură | Folosire |
| --- | --- |
| `tcp-listen:2323` | Ascultă pe calculatorul local |
| `tcp-listen:ADRESĂ:2323` | Ascultă pe interfața IPv4 dată |
| `tcp:127.0.0.1:2323` | Se conectează la un server |
| `pipe:NUME` sau `pipe:\\.\pipe\NUME` | Deschide același pipe local la ambele capete: `\\.\pipe\NUME` pe Windows, socketul `/tmp/NUME` pe Linux; primul proces îl servește |
| `COM3`, `/dev/ttyUSB0` sau `serial:COM3` | Deschide un port serial al calculatorului: pe Windows cu setările lui, pe Linux în mod brut |
| `loopback` | Întoarce datele transmise la receptor |
| `file:CALE` | Adaugă octeții transmiși la un fișier de captură |
| `none` | Terminalul rămâne neconectat sau ieșirea CLI merge în consolă |

De exemplu, în ZetRo se pune `ports.serial` pe `tcp-listen:2323` și se
pornește `daf2020.exe --serial tcp:127.0.0.1:2323`. Un model ZetRo cu port
serial, ca aMIC, poate folosi apoi terminalul din programele lui.

Pentru MezinXT cu adaptorul serial PC/XT se folosește configurația de mai
jos și se pornește `daf2020.exe --serial pipe:junior-daf`. Pe adaptorul SIO
al Junior-ului se folosește `ports.sio_a` pentru canalul A.

```yaml
ports:
  adapter: false
  pc_serial: one
  com1: 'pipe:\\.\pipe\junior-daf'
```

Recif și DAF-2020 folosesc implicit 9600 bauds, opt biți de date, fără
paritate și un bit de STOP. Alt format se dă cu `--serial-format 1200,7E1`
programului respectiv; pentru terminal, acesta pune comutatoarele S1–S7.
Vitezele de la 150 la 19200 bauds se dublează la fiecare pas. Formatul
acceptă 7 sau 8 biți de date, paritate N/E/O și 1 sau 2 biți de STOP. TCP
și pipe-urile transportă octeți de date bruți; fiecare emulator își face
singur încadrarea UART. Semnalele BREAK și de modem rămân locale.

`daf2020.exe --demo` arată grafică Tektronix, iar
`daf2020.exe --replay FLUX` afișează o captură serială. `--plain` oprește
efectul CRT.

## Imagini de bandă

Recif montează imagini SIMH TAP obișnuite, perechi de recuperare TDUP
(`.DAT` și `.LEN`) și capturile plate DEC BRU recunoscute. Cele două fișiere
ale unei perechi TDUP stau în același dosar. Un fișier de lungimi cu alt
nume se dă ca ultim argument pentru `tape-info`, `tape-convert`, `bru-list`
sau `bru-extract`.

Pentru o primă încercare se dezarhivează `benzi_20230817.tar.gz` și se
montează `CONVMIC/TAPEFULL.IMG`. Tasta **R** din monitor arată `VOL1BACKUP`.
Programul original din a doua înregistrare a benzii afișează mesajul de
volum neîncărcabil la pornirea cu **Încărcare de pe banda etichetată**.
`CORAL4/TAPEFULL.IMG` conține copiile CNC, JOCURI și PASCAL.
`benzi_20251107.tar.gz` conține și o copie TDUP a CONVMIC în `25/VARIANT1`.
[Colecțiile de benzi Z80-Romania](https://www.z80-romania.ro/Benzi_pentru_FELIX_CORAL_INDEPENDENT_etc_-170)

```powershell
.\recif.exe tape-info .\CONVMIC\TAPEFULL.IMG
.\recif.exe tape-convert .\CONVMIC\TAPEFULL.IMG .\convmic.tap
.\recif.exe bru-list .\CORAL4\TAPEFULL.IMG
.\recif.exe bru-extract .\CONVMIC\TAPEFULL.IMG .\convmic-files
.\recif.exe run --tape .\convmic.tap --boot-tape
```

Extragerea BRU scrie octeții fișierului până la sfârșitul de fișier
Files-11, cu încadrarea originală a înregistrărilor. Numele fișierului
cuprinde copia de siguranță, UIC-ul, numărul fișierului și versiunea.
Catalogul marchează ca parțiale fișierele cu blocuri lipsă sau
contradictorii. Când o captură plată își pierde alinierea înregistrărilor,
Recif montează partea lizibilă de la început și arată câți octeți rămân în
afara ei; un TAP exportat de acolo conține numai acea parte. Perechile TDUP
păstrează toate înregistrările și marcajele de fișier date. Conținutul
binar își păstrează codificarea originală, inclusiv EBCDIC.

Arhiva din martie 2023 conține o copie RSX41 împărțită. Se unesc ca date
binare `TEST0.TXT` și `banda0/DEST0.TXT`, în această ordine, apoi se
folosește `bru-list` sau `bru-extract` pe imaginea unită. Partea ei lizibilă
cuprinde două versiuni de `RSX11M.SYS`.

**Încărcare de pe banda etichetată** folosește un încărcător de referință
care sare peste eticheta de volum și execută a doua înregistrare. Pentru o
bandă fără etichete, `recif run --tape IMAGINE --boot-first` execută prima
înregistrare. Linia de comandă încarcă și un program PDP-11 plat cu
`--raw IMAGINE`, `--load OCTAL` și `--entry OCTAL`; adresa implicită de
încărcare și de start este `001000`.

## Limbă

Ambele programe sunt în engleză sau în română. Urmează limba sistemului până
se alege una în lista **Limbă** din panou sau în setările **F12** ale
terminalului.

## Compatibilitate

- **DAF-2020** urmează cartea tehnică din 1987: vectori Tektronix,
  caractere programabile, introducere grafică, text VT100/VT52,
  comutatoarele tastaturii și SET-UP. Folosește un set de caractere
  înlocuitor. Manualul dă lui SI și SO sensuri opuse în două locuri; Recif
  urmează paragraful 2.6.1, în care SI alege literele mari și este starea de
  la pornire. Interfețele de imprimantă și joystick nu sunt emulate, deci
  copia la imprimantă și modul transparent nu au efect.
  [Cartea tehnică FEPER DAF 2020](https://www.z80-romania.ro/1987_Carte_tehnica_DAF_2020-171)
- Copia RSX recuperată din aprilie 1989 nu pornește: încărcătorul ei nu este
  încărcabil, iar datele de bandă sunt deteriorate sau lipsesc. Banda RSX41FP
  se restaurează într-un sistem care pornește (vezi
  [pachetele de discuri](#pachete-de-discuri)).
- Programele Windows x64 au fost rulate pe un calculator real. Programele
  Windows ARM64 sunt compilate încrucișat și nu au fost rulate. Programele
  Linux au fost rulate numai fără fereastră, cele ARM64 sub emularea qemu:
  niciodată într-o fereastră pe un afișaj real, iar ieșirea lor de sunet
  deloc.
