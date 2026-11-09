"""The one error the reference raises for input the spec says to reject."""


class Rejected(ValueError):
    """Input that a rule of the spec rejects; `rule` is that rule's ID, such as "COB-5"."""

    def __init__(self, rule: str, reason: str):
        """
        Record the rejecting rule and its reason.

        :param rule: the rejecting rule's ID, such as "COB-5".
        :param reason: what was wrong with the input.
        """
        super().__init__(f'{rule}: {reason}')
        self.rule = rule
