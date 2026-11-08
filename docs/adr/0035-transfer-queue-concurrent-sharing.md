# 0035: Uploads im Hintergrund über eine Transfer-Queue mit gemeinsam genutzten Ressourcen

- **Status:** vorgeschlagen
- **Datum:** 2026-11-07

## Kontext

Bis R2.3 liefen alle Uploads auf der Grafik-Queue vor dem nächsten Frame, und Mip-Stufen rechnete
die GPU mit Blits (TD-17). Lädt der Editor ein Modell im Hintergrund, soll das Hochladen den Frame
nicht aufhalten. Viele GPUs haben eine eigene Queue-Familie nur für Kopien (DMA). Ressourcen, die
zwei Queue-Familien nutzen, brauchen entweder Übergaben (Release und Acquire auf beiden Queues)
oder gemeinsame Nutzung (`VK_SHARING_MODE_CONCURRENT`). Eine reine Transfer-Queue kann keine Blits.

## Entscheidung

- **Eigene Queue, wo es eine gibt:** Das Device sucht eine Familie mit Transfer, aber ohne Grafik
  und Compute, und legt eine zweite Queue an (`DeviceDesc::transfer_queue`, zum Testen
  abschaltbar; `Capabilities::transfer_queue`). Ohne sie läuft `upload_async()` wie `upload()`.
- **`upload_async()` mit Ticket:** Kopien sammeln sich in einem eigenen Staging-Ring und gehen bei
  der nächsten Abgabe an die Transfer-Queue; eine eigene Timeline Semaphore zählt sie. Das Ticket
  ist erledigt, wenn die CPU den Wert gesehen hat (`upload_done()`). Jede Abgabe der Grafik-Queue
  wartet auf den zuletzt gesehenen Wert; das kostet nichts und macht die Kopien sichtbar. Nur für
  neue Ressourcen, die die GPU noch nicht benutzt.
- **Gemeinsame Nutzung statt Übergaben:** Puffer im GPU-Speicher und Texturen mit `TransferDst`
  werden mit `VK_SHARING_MODE_CONCURRENT` für beide Familien angelegt, wenn es die Transfer-Queue
  gibt.
- **Mip-Stufen auf der CPU:** Der Lader rechnet alle Stufen im Hintergrund
  (`asset::mip_chain()`, in sRGB-Texturen linear gemittelt); `upload_async()` für Texturen nimmt
  die ganze Kette und braucht keinen Blit.
- **Freigeben:** Die Löschliste merkt sich auch den Stand der Transfer-Queue und gibt erst frei,
  wenn beide Queues fertig sind.
- **Renderer:** `GpuModel` trägt das Ticket seiner Uploads; der Renderer zeichnet ein Modell erst,
  wenn es erledigt ist, und zählt es bis dahin als „wartet“.

## Konsequenzen

- Hochladen großer Modelle blockiert keinen Frame mehr; nur `finish()` im Hauptthread legt
  Ressourcen an und kopiert in den Staging-Ring.
- Gemeinsame Nutzung kann auf manchen GPUs langsamer sein (keine Kompression von Texturen über
  Familien hinweg). Für Texturen aus Dateien ist das hinnehmbar; Render-Ziele bleiben exklusiv.
- lavapipe in der CI hat keine eigene Transfer-Familie; dort läuft der Rückfallpfad. Der Weg über
  die Transfer-Queue muss auf echten GPUs geprüft werden (Log „Transfer-Queue für Uploads im
  Hintergrund“).
- Mip-Stufen auf der CPU kosten Zeit im Hintergrund statt auf der GPU; mit KTX2 (R2.5) kommen sie
  fertig aus dem Cache.

## Erwogene Alternativen

- **Übergaben zwischen den Familien:** volle Leistung bei exklusiver Nutzung, aber jede Ressource
  braucht ein Release auf der Transfer-Queue und ein passendes Acquire in einer Abgabe der
  Grafik-Queue, die das Modell als erste nutzt. Das macht Uploads und Render-Graph abhängig
  voneinander.
- **Mip-Stufen weiter auf der GPU:** braucht nach der Kopie eine Abgabe auf der Grafik-Queue und
  damit doch wieder Übergaben.
- **Zweite Queue derselben Familie wie Grafik:** spart die gemeinsame Nutzung, nutzt aber nicht die
  DMA-Einheit und hilft bei der Last wenig.
