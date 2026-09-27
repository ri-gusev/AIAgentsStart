"""Public Codeforces API client; transport is injectable for offline tests."""

import json
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

CONTEST_LIST_URL = "https://codeforces.com/api/contest.list?gym=false"


class CodeforcesAPIError(Exception):
    """Network, API status or response validation error."""


def normalize_upcoming(contests: list) -> list[dict]:
    if not isinstance(contests, list):
        raise CodeforcesAPIError("Codeforces result must be a list of contests")
    upcoming = []
    seen = set()
    for contest in contests:
        if not isinstance(contest, dict) or not isinstance(contest.get("phase"), str):
            raise CodeforcesAPIError("Codeforces returned an invalid contest object")
        if contest["phase"] != "BEFORE":
            continue
        contest_id = contest.get("id")
        if type(contest_id) is not int or contest_id <= 0 or contest_id in seen:
            raise CodeforcesAPIError("Codeforces returned an invalid or duplicate contest ID")
        if any(not isinstance(contest.get(key), str) or not contest[key].strip() for key in ("name", "type")):
            raise CodeforcesAPIError("Codeforces contest is missing name or type")
        duration = contest.get("durationSeconds")
        if type(duration) is not int or duration < 0 or duration > 2**63 - 1:
            raise CodeforcesAPIError("Codeforces contest has invalid durationSeconds")
        start = contest.get("startTimeSeconds")
        if start is not None and (type(start) is not int or start < 0 or start > 2**63 - 1):
            raise CodeforcesAPIError("Codeforces contest has invalid startTimeSeconds")
        if contest_id > 2**63 - 1:
            raise CodeforcesAPIError("Codeforces contest ID is out of range")
        seen.add(contest_id)
        upcoming.append({"id": contest_id, "name": contest["name"], "type": contest["type"],
                         "phase": "BEFORE", "start_time_seconds": start, "duration_seconds": duration})
    return sorted(upcoming, key=lambda item: (
        item["start_time_seconds"] is None, item["start_time_seconds"] or 0, item["id"]))


class CodeforcesClient:
    def __init__(self, opener=urlopen, timeout: float = 15):
        self.opener = opener
        self.timeout = timeout

    def upcoming_contests(self) -> list[dict]:
        request = Request(CONTEST_LIST_URL, headers={
            "Accept": "application/json", "User-Agent": "AI-Agent-Course-Codeforces/1.0"})
        try:
            with self.opener(request, timeout=self.timeout) as response:
                payload = json.load(response)
        except HTTPError as error:
            error.close()
            raise CodeforcesAPIError(f"Codeforces HTTP error: {error.code}") from error
        except (URLError, OSError, TimeoutError) as error:
            raise CodeforcesAPIError("Codeforces API is unavailable or timed out") from error
        except (ValueError, UnicodeError) as error:
            raise CodeforcesAPIError("Codeforces API returned invalid JSON") from error
        if not isinstance(payload, dict):
            raise CodeforcesAPIError("Codeforces API returned an invalid response")
        if payload.get("status") == "FAILED":
            comment = payload.get("comment")
            detail = comment[:1000] if isinstance(comment, str) and comment else "No error comment"
            raise CodeforcesAPIError("Codeforces API FAILED: " + detail)
        if payload.get("status") != "OK":
            raise CodeforcesAPIError("Codeforces API response status is not OK")
        return normalize_upcoming(payload.get("result"))
