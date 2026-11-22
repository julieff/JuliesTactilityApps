# Slovakia Transit

Search Slovak bus and train connections together. Enter an origin and destination,
or use the stop-search buttons and select an exact stop. Date (`YYYY-MM-DD`)
and time (`HH:MM`) are optional; when omitted, the service uses its current
date and time. Results include each leg, available platforms and transfer details.
Stop suggestions and connections appear on a separate results page. Select a
stop to return to the search form, or use the back button to change the search.
After going back, the last results can be reopened from the form.

The app requires Wi-Fi, a correctly set device clock and firmware with the HTTPS
certificate bundle. It sends the entered places and optional date/time to
[CpSkAPI](https://cpskapi.lib.tuke.sk/index.html), an independent, unofficial
service that scrapes CP.sk. It can be unavailable or return incomplete or
incorrect data, especially around transfers and overnight trips. **Always verify
connections and platform information with [CP.sk](https://cp.sk/).**
This app is not affiliated with either service.
The service's usage policy and long-term availability have not been verified.

Build from the repository root with:

```powershell
python tt.py app build --path Apps\SlovakiaTransit --architecture esp32s3
```

The app targets the 0.8.0-dev SDK and matching firmware. Searches run on the app
task, not the UI thread; closing during a request may take until it times out.
