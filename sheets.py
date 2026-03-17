"""Google Sheets integration for tonGraph project context."""

import json
from pathlib import Path

from google.oauth2.service_account import Credentials
from googleapiclient.discovery import build

SPREADSHEET_ID = "14VwA6OGZ9A1IStozZS6Y-PO_40Cuq0yAFDgXka5_CHU"
CREDENTIALS_FILE = Path(__file__).parent / "sheets_credentials.json"
SCOPES = ["https://www.googleapis.com/auth/spreadsheets"]


def _get_service():
    creds = Credentials.from_service_account_file(str(CREDENTIALS_FILE), scopes=SCOPES)
    return build("sheets", "v4", credentials=creds).spreadsheets()


def read_sheet(range_: str = "Sheet1") -> list[list]:
    """Read data from spreadsheet. range_ example: 'Sheet1' or 'Sheet1!A1:D10'"""
    sheet = _get_service()
    result = sheet.values().get(spreadsheetId=SPREADSHEET_ID, range=range_).execute()
    return result.get("values", [])


def update_sheet(range_: str, values: list[list]) -> dict:
    """Write data to spreadsheet. values is a 2D list of rows."""
    sheet = _get_service()
    body = {"values": values}
    result = (
        sheet.values()
        .update(
            spreadsheetId=SPREADSHEET_ID,
            range=range_,
            valueInputOption="USER_ENTERED",
            body=body,
        )
        .execute()
    )
    return result


def append_rows(range_: str, values: list[list]) -> dict:
    """Append rows after existing data."""
    sheet = _get_service()
    body = {"values": values}
    result = (
        sheet.values()
        .append(
            spreadsheetId=SPREADSHEET_ID,
            range=range_,
            valueInputOption="USER_ENTERED",
            insertDataOption="INSERT_ROWS",
            body=body,
        )
        .execute()
    )
    return result


def get_context() -> str:
    """Read entire sheet and return as formatted string for use as LLM context."""
    rows = read_sheet()
    if not rows:
        return "(таблица пуста)"
    lines = []
    for row in rows:
        lines.append("\t".join(str(cell) for cell in row))
    return "\n".join(lines)


if __name__ == "__main__":
    import sys
    sys.stdout.reconfigure(encoding="utf-8")
    print("=== Содержимое таблицы ===")
    print(get_context())
