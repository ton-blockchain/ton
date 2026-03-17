# Google Sheets Integration

Spreadsheet: `14VwA6OGZ9A1IStozZS6Y-PO_40Cuq0yAFDgXka5_CHU`
Service account: `sheetapiservice@gen-lang-client-0052849427.iam.gserviceaccount.com`

## Setup on a new machine

1. **Get credentials JSON** from the project owner (`sheets_credentials.json`) — never commit this file.

2. **Place it** in the project root: `c:\GitHub\tonGraph\sheets_credentials.json`

3. **Install dependencies:**
   ```bash
   uv add google-auth google-auth-oauthlib google-api-python-client
   ```

4. **Test connection:**
   ```bash
   uv run python sheets.py
   ```
   Expected output: table contents printed to stdout.

## Usage

```python
from sheets import read_sheet, update_sheet, append_rows, get_context

context = get_context()           # full sheet as string (for LLM context)
data = read_sheet("Sheet1!A1:D20")
update_sheet("Sheet1!A2", [["value", "other"]])
append_rows("Sheet1", [["row1", "data"]])
```

## Notes

- `sheets_credentials.json` is in `.gitignore` — share it out-of-band (e.g. password manager)
- The service account already has Editor access to the spreadsheet — no additional sharing needed
