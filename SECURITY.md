# Security Policy

## Scope

This repository contains a C11 library with no I/O, no clock, no environment access.
Security issues are memory-safety bugs (buffer overflows, use-after-free, double-free)
or ABI contract violations that could cause crashes or undefined behavior.

## Reporting a Vulnerability

Please report security vulnerabilities to: bitsgenix@gmail.com

**Please do not report security vulnerabilities through public GitHub issues.**

You should receive a response within 48 hours. If for some reason you do not, please follow up to ensure we received your original message.

Please include the following information in your report:

- Type of issue (e.g. buffer overflow, use-after-free)
- Full paths of source file(s) related to the manifestation of the issue
- The location of the affected source code (tag/branch/commit or direct URL)
- Any special configuration required to reproduce the issue
- Step-by-step instructions to reproduce the issue
- Proof-of-concept or exploit code (if possible)
- Impact of the issue, including how an attacker might exploit it

## Preferred Languages

We prefer all communications to be in English.

## Policy

We follow the principle of Coordinated Vulnerability Disclosure.
