/* crypto_shims.c — OPTIONAL module extending the loader from no-crypto sensors
 * (like the FT9201) to SDCP-crypto Windows Hello sensors.
 *
 * NOT built for the FT9201. To use it for a crypto sensor, #include this file
 * inside ft_engine.c (after the shim table), call register_crypto_shims() from
 * register_shims(), and bump `shims[128]` to at least 256. It relies on
 * ft_engine.c's MS macro, integer typedefs, and reg().
 *
 * WHY THIS WORKS: the loader IS the host process, so every SDCP crypto gate that
 * exists to stop an *external* attacker tampering with the host engine is
 * neutralizable in-process. Two hooks carry the load; the rest just return
 * success with deterministic, well-formed outputs so the engine's init state
 * machine proceeds. Determinism matters: enroll and verify then see the SAME
 * derived key material, so anything that harmlessly folds session bytes in stays
 * self-consistent across the two operations.
 *
 * THE TWO THAT MATTER:
 *   BCryptDecrypt         -> PASSTHROUGH  (defeats the per-sample seal; the
 *                                          extractor consumes the plaintext image)
 *   BCryptVerifySignature -> SUCCESS      (defeats Attach-time device cert-chain
 *                                          verify against MS Root CA 2010)
 * Validated against a match-on-host engine whose extractor references ZERO crypto
 * imports (proven by disassembly): decrypt-then-extract with no per-sample MAC/sig
 * gating, so a substituted plaintext sample is not cryptographically rejected.
 *
 * PRINCIPLED ALTERNATIVE: against genuine, physically-owned silicon you can do the
 * REAL handshake (real ECDH, verify the real cert chain, real challenge-response)
 * instead of these bypasses. These shims are the offline / device-free path.
 *
 * HEAP NOTE: LocalAlloc/Free below use real free, matching ft_engine.c's Heap*
 * shims. Some crypto-engine static CRTs realloc-then-free the same block and will
 * double-free through glibc. Do NOT "fix" that with a leak-only allocator: it was
 * measured at ~2.7 MB leaked *per verify*, which OOMs any daemon. Use a
 * tracked-idempotent allocator instead (free for real, ignore double/foreign
 * frees) for Heap* AND Local*. See PORTING.md "Heap hardening".
 */
#define STATUS_SUCCESS 0u

/* LocalAlloc/Free/Size — crypto engines pull these in. Real free (see HEAP NOTE). */
static void* MS sh_LocalAlloc(u32 flags,u64 bytes){ void*p=malloc(bytes?bytes:1); if(p&&(flags&0x40)) memset(p,0,bytes); return p; }  /* flags&0x40 = LMEM_ZEROINIT */
static void* MS sh_LocalFree(void*h){ free(h); return 0; }
static u64   MS sh_LocalSize(void*h){ (void)h; return 0; }

static u32 MS sh_BCryptOpenAlgorithmProvider(void**ph,const u16*algid,const u16*impl,u32 fl){ if(ph)*ph=(void*)0xA16; return 0; }
static u32 MS sh_BCryptCloseAlgorithmProvider(void*h,u32 fl){ return 0; }
static u32 MS sh_BCryptSetProperty(void*h,const u16*prop,u8*in,u32 cb,u32 fl){ return 0; }
static u32 MS sh_BCryptGenRandom(void*h,u8*buf,u32 cb,u32 fl){ if(buf) for(u32 i=0;i<cb;i++) buf[i]=(u8)(0xAB^i); return 0; }  /* deterministic */
static u32 MS sh_BCryptGenerateKeyPair(void*h,void**pk,u32 len,u32 fl){ if(pk)*pk=(void*)0x4E1; return 0; }
static u32 MS sh_BCryptFinalizeKeyPair(void*k,u32 fl){ return 0; }
static u32 MS sh_BCryptGenerateSymmetricKey(void*h,void**pk,u8*ko,u32 cko,u8*sec,u32 csec,u32 fl){ if(pk)*pk=(void*)0x4E2; return 0; }
static u32 MS sh_BCryptImportKeyPair(void*h,void*ik,const u16*blob,void**pk,u8*in,u32 cb,u32 fl){ if(pk)*pk=(void*)0x4E3; return 0; }
static u32 MS sh_BCryptExportKey(void*k,void*ek,const u16*blob,u8*out,u32 cout,u32*res,u32 fl){
  u32 need=72; if(res)*res=need; if(out&&cout>=need) for(u32 i=0;i<need;i++) out[i]=(u8)(0x11+i); return 0; }
static u32 MS sh_BCryptSecretAgreement(void*priv,void*pub,void**psec,u32 fl){ if(psec)*psec=(void*)0x5EC; return 0; }
static u32 MS sh_BCryptDeriveKey(void*sec,const u16*kdf,void*params,u8*out,u32 cout,u32*res,u32 fl){
  if(res) *res=cout?cout:32;
  if(out&&cout) for(u32 i=0;i<cout;i++) out[i]=(u8)(0x5A+i);
  return 0; }   /* deterministic session key */
static u32 MS sh_BCryptCreateHash(void*h,void**ph,u8*ho,u32 cho,u8*sec,u32 csec,u32 fl){ if(ph)*ph=(void*)0x4A5; return 0; }
static u32 MS sh_BCryptHashData(void*h,u8*in,u32 cb,u32 fl){ return 0; }
static u32 MS sh_BCryptFinishHash(void*h,u8*out,u32 cb,u32 fl){ if(out) for(u32 i=0;i<cb;i++) out[i]=(u8)(0x33+i); return 0; }
static u32 MS sh_BCryptDestroyHash(void*h){ return 0; }
static u32 MS sh_BCryptSignHash(void*k,void*pad,u8*in,u32 cin,u8*out,u32 cout,u32*res,u32 fl){
  if(res) *res=cout?cout:64;
  if(out&&cout) for(u32 i=0;i<cout;i++) out[i]=(u8)(0x77+i);
  return 0; }
static u32 MS sh_BCryptVerifySignature(void*k,void*pad,u8*hash,u32 chash,u8*sig,u32 csig,u32 fl){ return STATUS_SUCCESS; }  /* cert-verify defeat */
static u32 MS sh_BCryptDestroyKey(void*k){ return 0; }
static u32 MS sh_BCryptDestroySecret(void*s){ return 0; }
static u32 MS sh_BCryptEncrypt(void*k,u8*in,u32 cin,void*pad,u8*iv,u32 civ,u8*out,u32 cout,u32*res,u32 fl){
  if(!out){ if(res)*res=cin; return 0; }                          /* size query */
  u32 n=cin<cout?cin:cout; if(in&&out) memcpy(out,in,n); if(res)*res=n; return 0; }   /* passthrough (template-seal round-trips) */
static u32 MS sh_BCryptDecrypt(void*k,u8*in,u32 cin,void*pad,u8*iv,u32 civ,u8*out,u32 cout,u32*res,u32 fl){
  if(!out){ if(res)*res=cin; return 0; }                          /* size query */
  u32 n=cin<cout?cin:cout; if(in&&out) memcpy(out,in,n); if(res)*res=n; return 0; }   /* PASSTHROUGH — the sample-seal defeat */

static void register_crypto_shims(void){
  reg("LocalAlloc",sh_LocalAlloc); reg("LocalFree",sh_LocalFree); reg("LocalSize",sh_LocalSize);
  reg("BCryptOpenAlgorithmProvider",sh_BCryptOpenAlgorithmProvider);
  reg("BCryptCloseAlgorithmProvider",sh_BCryptCloseAlgorithmProvider);
  reg("BCryptSetProperty",sh_BCryptSetProperty); reg("BCryptGenRandom",sh_BCryptGenRandom);
  reg("BCryptGenerateKeyPair",sh_BCryptGenerateKeyPair); reg("BCryptFinalizeKeyPair",sh_BCryptFinalizeKeyPair);
  reg("BCryptGenerateSymmetricKey",sh_BCryptGenerateSymmetricKey);
  reg("BCryptImportKeyPair",sh_BCryptImportKeyPair); reg("BCryptExportKey",sh_BCryptExportKey);
  reg("BCryptSecretAgreement",sh_BCryptSecretAgreement); reg("BCryptDeriveKey",sh_BCryptDeriveKey);
  reg("BCryptCreateHash",sh_BCryptCreateHash); reg("BCryptHashData",sh_BCryptHashData);
  reg("BCryptFinishHash",sh_BCryptFinishHash); reg("BCryptDestroyHash",sh_BCryptDestroyHash);
  reg("BCryptSignHash",sh_BCryptSignHash); reg("BCryptVerifySignature",sh_BCryptVerifySignature);
  reg("BCryptDestroyKey",sh_BCryptDestroyKey); reg("BCryptDestroySecret",sh_BCryptDestroySecret);
  reg("BCryptEncrypt",sh_BCryptEncrypt); reg("BCryptDecrypt",sh_BCryptDecrypt);
}
