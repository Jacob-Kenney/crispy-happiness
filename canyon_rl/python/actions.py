"""Map language-model tokens onto Canyon Racer mini-actions.

Every token in the vocabulary is classified once.  Tokens that decode to one
of the action words (modulo case / leading whitespace) are valid actions;
everything else is invalid: it is executed as "wait" and the agent is
penalized during PPO training.
"""

from functools import lru_cache
from typing import Dict, List

import torch

from canyon_env import ACTION_ALIASES, ACTION_NAMES


def action_table(
    tokenizer, device="cpu", vocab_size: int = None
) -> Dict[str, torch.Tensor]:
    """Return id->action / group / validity tensors for the model vocab.

    The tokenizer may be shorter than the model vocabulary (unused embedding
    rows); those ids can never be produced by the tokenizer but can be sampled
    from the LM head, so they are marked invalid.
    """
    if vocab_size is None:
        vocab_size = len(tokenizer)
    token_to_action: List[int] = []
    for token_id in range(vocab_size):
        text = tokenizer.decode([token_id]) if token_id < len(tokenizer) else ""
        token_to_action.append(ACTION_ALIASES.get(text.strip().lower(), -1))

    table = torch.tensor(token_to_action, dtype=torch.long)
    valid = table >= 0
    # group id per token; invalid tokens land in group len(ACTION_NAMES).
    group = torch.where(valid, table, torch.full_like(table, len(ACTION_NAMES)))
    return {
        "token_to_action": table.to(device),
        "valid_token_ids": torch.nonzero(valid, as_tuple=False).squeeze(-1).to(device),
        "valid_mask": valid.to(device),
        "group": group.to(device),
    }


def valid_bias(
    tokenizer, value: float, device="cpu", vocab_size: int = None
) -> torch.Tensor:
    """Constant logit bonus on valid action tokens.

    This is a fixed prior: it keeps early rollouts from being 100% invalid
    while still leaving plenty of probability mass for invalid tokens (which
    are penalized).
    """
    if vocab_size is None:
        vocab_size = len(tokenizer)
    tbl = action_table(tokenizer, vocab_size=vocab_size)["valid_mask"]
    bias = torch.zeros(vocab_size, dtype=torch.float32)
    bias[tbl] = float(value)
    return bias.to(device)


def decode_action(tokenizer, token_id: int) -> str:
    table = action_table(tokenizer)["token_to_action"]
    a = int(table[token_id])
    return ACTION_NAMES[a] if a >= 0 else "<invalid>"