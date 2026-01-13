# Creating a Security Policy

This guide shows you how to configure security policies to control which RDP destinations your users can connect to.

---

## Why Use a Security Policy?

Without a security policy, users can connect to **any** RDP host. A security policy lets you:

- Restrict connections to approved servers only
- Allow only specific IP ranges (e.g., your internal network)
- Enforce port restrictions
- Block connections to unauthorized destinations

---

## Quick Start

### Frontend Only

Pass a `securityPolicy` when creating your RDP client:

```javascript
const client = new RDPClient(container, {
    wsUrl: 'wss://your-server.com',
    securityPolicy: {
        allowedHostnames: ['*.internal.corp'],
        allowedIpv4Cidrs: ['10.0.0.0/8']
    }
});
```

### Backend Only

Create a JSON file at `/app/security/rdp-bridge-policy.json`:

```json
{
    "allowedHostnames": ["*.internal.corp"],
    "allowedIpv4Cidrs": ["10.0.0.0/8"]
}
```

### Both (Recommended)

For maximum security, configure **both** frontend and backend with matching policies. This provides defense-in-depth — even if someone bypasses the frontend, the backend will still block unauthorized connections.

---

## Policy Options

A security policy has three optional properties. You can use any combination:

| Property | What It Does | Best For |
|----------|--------------|----------|
| `allowedHostnames` | Match hostnames using wildcards | Domain-based restrictions |
| `allowedIpv4Cidrs` | Match IP addresses using CIDR ranges | Network-based restrictions |
| `allowedDestinationRegex` | Match `host:port` using regex | Complex rules or port restrictions |

> **Important**: If you define at least one rule, only matching destinations are allowed. If no rules are defined (or the policy is empty), all connections are permitted.

### How Rules Work Together

Rules use **OR logic** — a connection is allowed if it matches **any** rule:

```javascript
securityPolicy: {
    allowedHostnames: ['*.corp'],        // Matches? Allow!
    allowedIpv4Cidrs: ['10.0.0.0/8'],    // OR matches? Allow!
    allowedDestinationRegex: ['^backup:3390$']  // OR matches? Allow!
}
```

---

## Hostname Patterns (`allowedHostnames`)

Use glob patterns to match hostnames. These patterns **only apply to hostnames**, not IP addresses.

### Pattern Syntax

| Pattern | Matches | Example |
|---------|---------|---------|
| `*` | Any characters | `*.corp` → `web.corp`, `db.prod.corp` |
| `?` | One character | `srv?.corp` → `srv1.corp`, `srvA.corp` |
| `[abc]` | Any of a, b, c | `db[123].corp` → `db1.corp`, `db2.corp` |
| `[!abc]` | Not a, b, or c | `db[!0].corp` → `db1.corp` but not `db0.corp` |

### Examples

```javascript
securityPolicy: {
    allowedHostnames: [
        '*.internal.mycompany.com',    // Any subdomain
        'prod-*.mycompany.com',        // Anything starting with "prod-"
        'server[1-5].corp',            // server1 through server5
        'rdp.example.com'              // Exact match
    ]
}
```

> **Note**: Hostname matching is case-insensitive.

---

## IP Ranges (`allowedIpv4Cidrs`)

Use CIDR notation to match IP address ranges. These rules **only apply to IP addresses**, not hostnames.

### CIDR Basics

CIDR notation looks like `IP/prefix`. The prefix is the number of fixed bits:

| CIDR | Range | Addresses |
|------|-------|-----------|
| `10.0.0.0/8` | 10.0.0.0 – 10.255.255.255 | ~16 million |
| `172.16.0.0/12` | 172.16.0.0 – 172.31.255.255 | ~1 million |
| `192.168.0.0/16` | 192.168.0.0 – 192.168.255.255 | ~65,000 |
| `192.168.1.0/24` | 192.168.1.0 – 192.168.1.255 | 256 |
| `10.50.100.25/32` | 10.50.100.25 only | 1 |

### Examples

```javascript
securityPolicy: {
    allowedIpv4Cidrs: [
        '10.0.0.0/8',           // All 10.x.x.x addresses
        '172.16.0.0/12',        // All 172.16-31.x.x addresses
        '192.168.1.0/24',       // Just the 192.168.1.x subnet
        '203.0.113.50/32'       // Single specific IP
    ]
}
```

---

## Regex Patterns (`allowedDestinationRegex`)

Use regular expressions to match the full `host:port` string. This is the most flexible option and works with both hostnames and IPs.

### When to Use Regex

- You need to restrict by **port number**
- You need complex matching logic
- You want a single rule for both hostnames and IPs

### Examples

```javascript
securityPolicy: {
    allowedDestinationRegex: [
        // Only allow the standard RDP port
        '^.*:3389$',
        
        // Numbered servers (prod-server-01 through prod-server-99)
        '^prod-server-\\d+:3389$',
        
        // Specific IP range on ports 3380-3399
        '^192\\.168\\.1\\.\\d+:33[89]\\d$',
        
        // Specific hosts only
        '^(web|app|db)\\.corp:3389$'
    ]
}
```

### Escaping in JavaScript

In JavaScript strings, backslash needs double-escaping:

| Regex | JavaScript String |
|-------|-------------------|
| `\.` | `'\\.'` |
| `\d` | `'\\d'` |
| `\\` | `'\\\\'` |

### Escaping in JSON

Same rule applies in JSON files:

```json
{
    "allowedDestinationRegex": [
        "^192\\.168\\.1\\.\\d+:3389$"
    ]
}
```

---

## Frontend Integration

### Basic Setup

```javascript
import { RDPClient } from './rdp-client.js';

const client = new RDPClient(document.getElementById('rdp-container'), {
    wsUrl: 'wss://rdp-proxy.example.com',
    securityPolicy: {
        allowedHostnames: ['*.internal.corp'],
        allowedIpv4Cidrs: ['10.0.0.0/8', '192.168.0.0/16']
    }
});
```

### Check Before Connecting

You can validate a destination before attempting to connect:

```javascript
const result = client.validateDestination('10.50.100.25', 3389);

if (result.allowed) {
    // Safe to connect
    client.connect({ host: '10.50.100.25', user: 'admin', pass: 'secret' });
} else {
    // Show error to user
    alert('Connection blocked: ' + result.reason);
}
```

### Get the Current Policy

```javascript
const policy = client.getSecurityPolicy();
console.log(policy.allowedHostnames);
// Note: The returned object is frozen and cannot be modified
```

### Connection Errors

When a connection is blocked, the `connect()` promise rejects:

```javascript
client.connect({ host: 'unauthorized-host.com', user: 'x', pass: 'x' })
    .catch(err => {
        console.error(err.message);
        // "Connection to unauthorized-host.com:3389 blocked by security policy"
    });
```

---

## Backend Integration

The backend validates connections server-side using the same policy format. **Backend policies are essential** — they cannot be bypassed by users, unlike frontend-only policies.

### Option 1: Volume Mount (Recommended)

Mount a local directory containing your policy file into the container:

```bash
# Create a local policy file
mkdir -p ./security
cat > ./security/rdp-bridge-policy.json << 'EOF'
{
    "allowedHostnames": ["*.internal.corp"],
    "allowedIpv4Cidrs": ["10.0.0.0/8", "192.168.1.0/24"]
}
EOF

# Run with volume mount
docker run -d \
    -p 8765:8765 \
    -v $(pwd)/security:/app/security:ro \
    qxsch/freerdpwebbackend:latest
```

Or in `docker-compose.yml`:

```yaml
services:
  backend:
    image: qxsch/freerdpwebbackend:latest
    ports:
      - "8765:8765"
    volumes:
      - ./security:/app/security:ro
```

You can also use a custom path with the environment variable:

```yaml
services:
  backend:
    image: qxsch/freerdpwebbackend:latest
    ports:
      - "8765:8765"
    volumes:
      - ./my-policies:/etc/rdp-policies:ro
    environment:
      - RDP_BRIDGE_SECURITY_POLICY_PATH=/etc/rdp-policies/production-policy.json
```

### Option 2: Custom Dockerfile

Create a custom Docker image with the policy baked in:

```dockerfile
FROM qxsch/freerdpwebbackend:latest

RUN mkdir -p /app/security/
COPY rdp-bridge-policy.json /app/security/
```

Build and run:

```bash
# Create your policy file
cat > rdp-bridge-policy.json << 'EOF'
{
    "allowedHostnames": ["*.mycompany.com"],
    "allowedIpv4Cidrs": ["172.16.0.0/12"],
    "allowedDestinationRegex": ["^jumpbox:3389$"]
}
EOF

# Build custom image
docker build -t my-rdp-backend .

# Run
docker run -d -p 8765:8765 my-rdp-backend
```

### Backend Behavior

| Scenario | Result |
|----------|--------|
| No policy file exists | All connections allowed |
| Policy file is empty `{}` | All connections allowed |
| Policy has rules | Only matching destinations allowed |
| Connection blocked | Client receives error with `security_policy_violation` |

---

## Complete Examples

### Corporate Network

Allow only internal resources:

```javascript
// Frontend
const client = new RDPClient(container, {
    wsUrl: 'wss://rdp-proxy.mycompany.com',
    securityPolicy: {
        allowedHostnames: [
            '*.internal.mycompany.com',
            '*.prod.mycompany.com'
        ],
        allowedIpv4Cidrs: [
            '10.0.0.0/8',
            '172.16.0.0/12'
        ]
    }
});
```

```json
// Backend: /app/security/rdp-bridge-policy.json
{
    "allowedHostnames": [
        "*.internal.mycompany.com",
        "*.prod.mycompany.com"
    ],
    "allowedIpv4Cidrs": [
        "10.0.0.0/8",
        "172.16.0.0/12"
    ]
}
```

### Specific Servers Only

Lock down to known machines:

```javascript
securityPolicy: {
    allowedHostnames: [
        'jumpbox.dmz.corp',
        'devserver.internal.corp',
        'prodserver.internal.corp'
    ]
}
```

### Port Restrictions

Allow any internal host, but only on port 3389:

```javascript
securityPolicy: {
    allowedDestinationRegex: [
        '^10\\.\\d+\\.\\d+\\.\\d+:3389$',
        '^192\\.168\\.\\d+\\.\\d+:3389$'
    ]
}
```

### Development (Allow All)

For local development only — **don't use in production**:

```javascript
// No securityPolicy = allow all
const client = new RDPClient(container, {
    wsUrl: 'ws://localhost:8765'
});
```

---

## Security Considerations

1. **Always enable backend policies** — At minimum, configure a backend security policy. Frontend policies alone are insufficient because they can be bypassed by users who modify the JavaScript or use browser developer tools.

2. **Use both frontend and backend policies** — Frontend policies provide a better user experience (instant feedback and in code validation method), while backend policies provide actual security.

3. **Be specific** — Avoid overly broad patterns like `*` or `0.0.0.0/0`

4. **Use HTTPS/WSS** — Always use secure WebSocket (`wss://`) in production

5. **Mount read-only** — Use `:ro` when mounting policy files to prevent modification

6. **Policies are immutable** — Once created, the policy cannot be changed at runtime (by design). This prevents accidental misconfiguration, but does not guarantee security against a determined attacker, that recreates the container with a different policy. (See frontend considerations above.)

---

## Troubleshooting

### "Connection blocked by security policy"

Your destination doesn't match any rule. Check:
- Is the hostname spelled correctly?
- Is the IP in your allowed CIDR range?
- Did you forget to add a rule?

### Rules aren't matching

- **Hostnames**: Patterns only match hostnames, not IPs
- **CIDRs**: Only match IP addresses, not hostnames
- **Regex**: Must match the full `host:port` string

### Backend policy not loading

1. Check the file exists: `docker exec <container> cat /app/security/rdp-bridge-policy.json`
2. Check for JSON syntax errors
3. Check the logs: `docker logs <container>`

### Testing your regex

Test patterns at [regex101.com](https://regex101.com/) before deploying.

---

## Summary

| Want to... | Use |
|------------|-----|
| Allow `*.corp` domains | `allowedHostnames: ['*.corp']` |
| Allow your internal network | `allowedIpv4Cidrs: ['10.0.0.0/8']` |
| Restrict to port 3389 only | `allowedDestinationRegex: ['^.*:3389$']` |
| Allow everything | Don't set `securityPolicy` |
| Maximum security | Configure both frontend and backend |
