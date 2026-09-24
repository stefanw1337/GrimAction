# Test av musepeker — før publisering

Dette er en lokal testversjon. Ingen fork er publisert.

1. Avslutt spillet helt dersom gammel GrimAction er lastet inn. Ikke start begge versjoner i samme spilløkt.
2. Start Grim Dawn i DirectX 11 og vent til hovedmenyen.
3. Start denne pakkens «Start GrimAction.cmd» én gang. Ikke kopier DLL-en inn i spillmappen.
4. I verden: F8 på skal skjule pekeren helt. Ingen prikk. F8 av skal vise vanlig peker igjen.
5. Test Escape, inventory og NPC-samtale mens F8 er på. Pekeren skal vises og kunne klikke. Ved retur til aktiv kamerastyring skal den skjules igjen.
6. Hold Alt: pekeren skal vises. Slipp Alt: skjult igjen. Test også Alt-Tab og tilbake.
7. Gjenta F8 av/på flere ganger. Test «Stop GrimAction.cmd» mens pekeren er skjult.

Ved feil: noter hvilket steg som feiler og behold CSV-filen i logs. cursor_visibility=1 betyr at skjuling ble forsøkt på riktig tråd, ikke at et skjermbilde har bekreftet resultatet. Normal avslutning av spillet fjerner den innlastede test-DLL-en. Start en ny økt med gammel GrimAction for å gå tilbake.

Forventede begrensninger: DX9 er ikke endret. Upstream dekker ikke automatisk alle vinduer (bl.a. devotion, blacksmith, rift gates og death screen); Alt frigjør pekeren. Test disse også før publisering. Vi må ikke markedsføre støtte som ikke er bekreftet.
