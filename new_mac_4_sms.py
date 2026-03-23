import hmac, hashlib
key = bytes.fromhex("B584465480C5C5E5EBA141E74367D3A7BF668D8005511C1A43C9D9A9E33F1550")
devid = "ZKUCA5Y9"
ctr = 2  # must be > SMS? CTR
msg = f"FM1|REBOOT|{ctr}|{devid}"
mac16 = hmac.new(key, msg.encode("ascii"), hashlib.sha256).hexdigest()[:16]
print("SMS body: FM1 REBOOT", ctr, mac16)
