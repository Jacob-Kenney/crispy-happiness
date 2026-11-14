"""The language-model agent: LFM2.5-230M + grouped token action policy.

Policy structure
----------------
* LFM2.5-230M reads a system prompt plus one user message with the game state.
* The LM's next-token distribution is reduced to seven action logits by
  log-sum-exp over the valid action tokens (left/right/up/down/a/b/wait).
  Each valid action has several spelling/case variants in the vocabulary.
* A small trainable 7-vector bias (action prior) and a scalar value head are
  added on top.
* The policy samples one of the seven actions; the emitted LM token would be
  the highest-probability valid token inside that group.
* Any probability the LM puts on other tokens ("anything else") is measured as
  `invalid_mass` and penalized, both in the reward and as a differentiable
  regularizer.

The LM is adapted with LoRA (rank 8).  Full-model Adam steps on a 65k-way
softmax move logits by ~20 nats per step; LoRA at the learning rates here
changes them by ~0.1 nat per step, which is what PPO needs.
"""

from typing import Dict, List

import torch
import torch.nn.functional as F
from transformers import AutoModelForCausalLM

from actions import action_table

SYSTEM_PROMPT = """You pilot a ship up a narrow canyon in Canyon Racer.
Walls, rocks and items scroll toward you.
Reply with exactly one action word:
left, right, up, down, a (jump), b (save), wait (release buttons)
Hold a to jump higher; while airborne you cannot crash.
Other words are invalid and penalized."""

ACTION_NAMES = ["wait", "left", "right", "up", "down", "a", "b"]


def lora_config(rank=8, alpha=16):
    from peft import LoraConfig

    return LoraConfig(
        r=rank,
        lora_alpha=alpha,
        lora_dropout=0.0,
        target_modules="all-linear",
        bias="none",
        task_type="CAUSAL_LM",
    )


def build_lm(model_name: str, device: str, use_lora: bool = True, rank: int = 8):
    lm = AutoModelForCausalLM.from_pretrained(model_name, dtype=torch.float32)
    if use_lora:
        from peft import get_peft_model

        lm = get_peft_model(lm, lora_config(rank=rank))
        for name, p in lm.named_parameters():
            p.requires_grad = "lora_" in name
    else:
        for p in lm.parameters():
            p.requires_grad = True
    return lm.to(device)


class ActionSpace:
    """Grouped token space: many vocabulary tokens -> seven actions."""

    def __init__(self, tokenizer, vocab_size: int, device):
        tbl = action_table(tokenizer, vocab_size=vocab_size)
        valid = tbl["valid_mask"]
        ids = torch.nonzero(valid, as_tuple=False).squeeze(-1)
        groups = tbl["token_to_action"][ids]
        order = torch.argsort(groups)
        self.valid_ids = ids[order].to(device)
        self.valid_groups = groups[order].to(device)
        self.n_actions = len(ACTION_NAMES)
        self.device = device

    def group_logits(self, logits: torch.Tensor) -> torch.Tensor:
        """[B, V] token logits -> [B, 7] log-sum-exp per action group."""
        vl = logits.float()[:, self.valid_ids]
        mx = vl.max(dim=1, keepdim=True).values
        sums = torch.zeros(
            vl.shape[0], self.n_actions, dtype=vl.dtype, device=vl.device
        )
        sums.index_add_(1, self.valid_groups, torch.exp(vl - mx))
        return sums.log() + mx

    def invalid_mass(self, logits: torch.Tensor):
        """Probability the LM assigns to non-action tokens.

        Measured on the raw token distribution (the trainable action prior is
        not part of the LM's token probabilities).
        """
        gl = self.group_logits(logits)
        log_z = logits.float().logsumexp(dim=-1)
        log_valid = torch.logsumexp(gl, dim=-1)
        return (1.0 - torch.exp(log_valid - log_z)).clamp(0.0, 1.0)


class Policy(torch.nn.Module):
    def __init__(self, lm, hidden_size: int, action_space: ActionSpace,
                 prior=(0.0, -4.5, 0.0, 0.0, 0.0, 0.0, 0.0)):
        super().__init__()
        self.lm = lm
        self.action_space = action_space
        self.value_norm = torch.nn.LayerNorm(hidden_size)
        self.value_head = torch.nn.Linear(hidden_size, 1)
        torch.nn.init.zeros_(self.value_head.weight)
        torch.nn.init.zeros_(self.value_head.bias)
        # Trainable state-independent prior over the seven actions.
        self.action_prior = torch.nn.Parameter(torch.tensor(prior, dtype=torch.float32))

    def forward(self, input_ids, attention_mask):
        out = self.lm(
            input_ids=input_ids,
            attention_mask=attention_mask,
            use_cache=False,
            output_hidden_states=True,
            # Only the last position's logits are needed; this avoids
            # materializing a [B, seq, 65536] tensor.
            logits_to_keep=1,
        )
        # Left padding: last position is the last real token for every row.
        logits = out.logits[:, -1, :]
        hidden = out.hidden_states[-1][:, -1, :]
        gl = self.action_space.group_logits(logits) + self.action_prior
        value = self.value_head(self.value_norm(hidden)).squeeze(-1)
        return logits, gl, value


class PromptBuilder:
    def __init__(self, tokenizer, device):
        self.tok = tokenizer
        self.device = device
        self.pad_id = tokenizer.pad_token_id
        if self.pad_id is None:
            self.pad_id = tokenizer.eos_token_id or 0

    def encode(self, obs: str, last_action: str) -> List[int]:
        messages = [
            {"role": "system", "content": SYSTEM_PROMPT},
            {
                "role": "user",
                "content": (
                    f"State:\n{obs}\nlast={last_action}\n"
                    "Reply with one word: left, right, up, down, a, b, wait."
                ),
            },
        ]
        enc = self.tok.apply_chat_template(
            messages, add_generation_prompt=True, tokenize=True
        )
        return enc["input_ids"]

    def batch(self, texts: List[str], lasts: List[str]):
        seqs = [self.encode(t, l) for t, l in zip(texts, lasts)]
        maxlen = max(len(s) for s in seqs)
        ids = torch.full(
            (len(seqs), maxlen), self.pad_id, dtype=torch.long, device=self.device
        )
        mask = torch.zeros((len(seqs), maxlen), dtype=torch.long, device=self.device)
        for i, s in enumerate(seqs):
            ids[i, -len(s):] = torch.tensor(s, dtype=torch.long, device=self.device)
            mask[i, -len(s):] = 1
        return ids, mask


def save_policy(policy: Policy, path: str, cfg: dict, model_name: str, meta=None):
    """Save only the trainable parts (LoRA + heads); the base model is reused."""
    sd = {
        k: v.detach().cpu()
        for k, v in policy.state_dict().items()
        if "lora_" in k
        or k.startswith(("value_head", "value_norm"))
        or k == "action_prior"
    }
    torch.save(
        {
            "state_dict": sd,
            "cfg": cfg,
            "system_prompt": SYSTEM_PROMPT,
            "base_model": model_name,
            "meta": meta or {},
        },
        path,
    )


def load_policy_state(policy: Policy, path: str):
    """Load trainable weights into an existing policy; returns the checkpoint."""
    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    _, unexpected = policy.load_state_dict(ckpt["state_dict"], strict=False)
    if unexpected:
        raise RuntimeError(f"unexpected checkpoint keys: {unexpected[:5]}")
    return ckpt


def load_policy(path: str, device: str):
    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    model_name = ckpt["base_model"]
    cfg = ckpt["cfg"]
    lm = build_lm(model_name, device, use_lora=cfg.get("use_lora", True),
                  rank=cfg.get("lora_rank", 8))
    from transformers import AutoTokenizer

    tok = AutoTokenizer.from_pretrained(model_name)
    space = ActionSpace(tok, lm.config.vocab_size, device)
    policy = Policy(lm, lm.config.hidden_size, space,
                    prior=cfg.get("action_prior", (0.0, -4.5, 0.0, 0.0, 0.0, 0.0, 0.0))).to(device)
    _, unexpected = policy.load_state_dict(ckpt["state_dict"], strict=False)
    # Base-model weights are intentionally not stored in the checkpoint.
    if unexpected:
        raise RuntimeError(f"unexpected checkpoint keys: {unexpected[:5]}")
    policy.eval()
    return policy, tok, cfg