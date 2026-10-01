# -*- coding: utf-8 -*-
"""Sample Python service, used to exercise the Python extractor.

Deliberately includes the constructs that trip a naive parser:
  * a class with methods and self.method() dispatch
  * module-level functions
  * a nested function (closure)
  * decorators
  * a multi-line signature
  * calls inside comprehensions and f-strings
"""
import os
import json
from dataclasses import dataclass


@dataclass
class Money:
    cents: int

    def format(self) -> str:
        whole = self.cents // 100
        frac = abs(self.cents) % 100
        return f"{whole}.{frac:02d}"

    def is_negative(self) -> bool:
        return self.cents < 0


def normalize_email(email: str) -> str:
    return email.strip().lower()


class UserService:
    def __init__(self, path: str = "users.json"):
        self.path = path
        self.users = {}
        self.load()

    def load(self) -> int:
        """Read the user file; returns how many records were loaded."""
        if not os.path.exists(self.path):
            return 0
        with open(self.path, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
        for record in raw.get("users", []):
            self.add_user(
                record["id"],
                normalize_email(record["email"]),
            )
        return len(self.users)

    def add_user(self, user_id: str, email: str) -> bool:
        if not user_id:
            return False
        if user_id in self.users:
            return False
        self.users[user_id] = {"id": user_id, "email": email}
        return True

    def find(self, user_id: str):
        return self.users.get(user_id)

    def summary(self) -> str:
        # nested function: a closure over self
        def render(entry):
            money = Money(cents=1000)
            return entry["id"] + " " + money.format()

        parts = [render(u) for u in self.users.values()]
        return ", ".join(parts)

    def count(self) -> int:
        return len(self.users)


class AccountService:
    def __init__(self, users: UserService):
        self.users = users
        self.accounts = {}

    def open_account(self, account_id: str, user_id: str, cents: int) -> bool:
        if self.users.find(user_id) is None:
            return False
        self.accounts[account_id] = Money(cents=cents)
        return True

    def total(self) -> Money:
        total = Money(cents=0)
        for money in self.accounts.values():
            total = Money(cents=total.cents + money.cents)
        return total

    def statement(self, account_id: str) -> str:
        money = self.accounts.get(account_id)
        if money is None:
            return "unknown account"
        return money.format()


def build_default_services():
    users = UserService("users.json")
    accounts = AccountService(users)
    accounts.open_account("a1", "u1", 5000)
    return users, accounts


def main():
    users, accounts = build_default_services()
    print(users.summary())
    print(accounts.statement("a1"))
    print(accounts.total().format())


if __name__ == "__main__":
    main()
