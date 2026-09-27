"""Pure processing and presentation of MCP arguments; no database access."""

from datetime import datetime, timedelta, timezone
from typing import Annotated

from pydantic import BaseModel, Field

try:
    from .reminder_store import MOSCOW_TIMEZONE
except ImportError:
    from reminder_store import MOSCOW_TIMEZONE

PositiveInteger = Annotated[int, Field(strict=True, ge=1)]


class ReminderItem(BaseModel):
    id: PositiveInteger
    text: Annotated[str, Field(min_length=1, max_length=2000)]
    run_at: str
    status: str


class UpcomingReminders(BaseModel):
    range_days: PositiveInteger
    reminders: list[ReminderItem]


class SummaryItem(BaseModel):
    id: PositiveInteger
    text: Annotated[str, Field(min_length=1, max_length=2000)]
    run_at: str


class ReminderSummary(BaseModel):
    period_hours: PositiveInteger
    count: Annotated[int, Field(strict=True, ge=0)]
    items: list[SummaryItem]


def parse_time(value: str) -> datetime:
    moment = datetime.fromisoformat(value.replace("Z", "+00:00"))
    # Same interpretation as create_reminder: unqualified times mean Moscow.
    return moment.replace(tzinfo=MOSCOW_TIMEZONE) if moment.tzinfo is None else moment


def summarize(reminders: UpcomingReminders, hours: int = 24) -> dict:
    if type(hours) is not int or hours < 1:
        raise ValueError("hours must be a positive integer")
    now = datetime.now(timezone.utc)
    end = now + timedelta(hours=hours)
    selected = []
    for item in reminders.reminders:
        moment = parse_time(item.run_at).astimezone(timezone.utc)
        if item.status == "pending" and now < moment <= end:
            selected.append((moment, item.id, item.text))
    selected.sort(key=lambda item: (item[0], item[1]))
    return {"period_hours": hours, "count": len(selected), "items": [
        {"id": reminder_id, "text": text,
         "run_at": moment.isoformat().replace("+00:00", "Z")}
        for moment, reminder_id, text in selected
    ]}


def build_view(summary: ReminderSummary) -> dict:
    if summary.count != len(summary.items):
        raise ValueError("summary.count must match the number of items")
    items = []
    for item in summary.items:
        moment = parse_time(item.run_at).astimezone(MOSCOW_TIMEZONE)
        items.append({"id": item.id, "title": item.text,
                      "date": moment.strftime("%d.%m.%Y"), "time": moment.strftime("%H:%M")})
    return {"title": "Ближайшие планы", "count": summary.count,
            "message": f"Напоминаний на ближайшие {summary.period_hours} ч.: {summary.count}",
            "items": items}
