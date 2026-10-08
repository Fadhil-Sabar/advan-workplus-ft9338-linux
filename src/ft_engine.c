// loader.c — native in-process loader for ftWbioEngineAdapter.dll, NO WINE.
// Maps the PE, sets up a fake x64 TEB in %gs, runs static-CRT DllMain, then
// calls WbioQueryEngineInterface + Attach + AcceptSampleData on a real print.
//
// Build: gcc -O0 -g -o loader loader.c
// x86-64 Linux: glibc TLS uses %fs, so %gs is free for the Windows TEB.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define MS __attribute__((ms_abi))
#define DLL_PROCESS_ATTACH 1

static u8 *g_image;          // mapped image base
static u64 g_imagebase;      // preferred base 0x180000000
static const char *g_dllpath;

// ---------- tiny PE structures (only fields we use) ----------
#pragma pack(push,1)
typedef struct { u16 e_magic; u8 pad[58]; u32 e_lfanew; } DOS;
typedef struct {
  u32 Signature; u16 Machine, NumSecs; u32 TimeStamp, PtrSym, NumSym; u16 OptSize, Chars;
} FILEHDR;
typedef struct { u8 Name[8]; u32 VSize, VAddr, RawSize, RawPtr; u32 pad[3]; u16 pad2[2]; u32 Chars; } SEC;
#pragma pack(pop)

static u8 *g_file; static size_t g_filelen;
static u32 f32(u64 o);
static u16 f16(u64 o);
static int file_range(u64 o,u64 n){ return o<=g_filelen && n<=g_filelen-o; }
static u32 g_image_size;
static int image_writable_range(u64 o,u64 n);
static size_t readable_range_length(u64 o);
/* Require a request to fit wholly within one mapped region; SizeOfImage also
 * contains alignment gaps that intentionally remain PROT_NONE. */
static int image_range(u64 o,u64 n){
  if(!g_image || o>g_image_size || n>g_image_size-o) return 0;
  if(!n) return 1;
  u64 end=o+n;
  u64 cursor=o;
  while(cursor<end){
    u64 region_end=0;
    u64 hdrmap=0;
    /* Header mapping is rounded to pages; section ranges are similarly
     * rounded and may overlap due to PE section alignment. */
    u32 e=f32(0x3c), opt=e+24;
    u32 hdrsize=f32(opt+60);
    hdrmap=((u64)hdrsize+0xfff)&~0xfffULL;
    if(cursor<hdrmap) region_end=hdrmap;
    else {
      u16 nsec=f16(e+6), optsize=f16(e+20);
      u64 sectbl=(u64)e+24+optsize;
      for(u32 i=0;i<nsec;i++){
        u64 s=sectbl+(u64)i*40;
        u64 va=f32(s+12), span=f32(s+8), raw=f32(s+16);
        if(raw>span) span=raw;
        u64 rounded=(span+0xfff)&~0xfffULL;
        if(cursor>=va && cursor<va+rounded && va+rounded>region_end) region_end=va+rounded;
      }
    }
    if(region_end<=cursor) return 0;
    cursor=region_end<end?region_end:end;
  }
  return 1;
}
static int image_writable_range(u64 o,u64 n){
  if(!image_range(o,n)) return 0;
  u64 end=o+n;
  u32 e=f32(0x3c), optsize=f16(e+20), nsec=f16(e+6);
  u64 sectbl=(u64)e+24+optsize;
  for(u64 cursor=o;cursor<end;){
    int covered=0; u64 best=cursor;
    for(u32 i=0;i<nsec;i++){
        u64 s=sectbl+(u64)i*40, va=f32(s+12), span=f32(s+8), raw=f32(s+16), ch=f32(s+36);
      if(raw>span) span=raw;
      u64 lim=va+((span+0xfff)&~0xfffULL);
      if(cursor>=va && cursor<lim){ covered=1; if(lim>best) best=lim; }
      (void)ch;
    }
    if(!covered || best<=cursor) return 0;
    cursor=best<end?best:end;
  }
  return 1;
}
static size_t readable_range_length(u64 o){
  if(!image_range(o,1)) return 0;
  u32 e=f32(0x3c), optsize=f16(e+20), nsec=f16(e+6);
  u64 sectbl=(u64)e+24+optsize, limit=0;
  u32 hdrsize=f32((u64)e+24+60);
  u64 hdrmap=((u64)hdrsize+0xfff)&~0xfffULL;
  if(o<hdrmap) limit=hdrmap;
  for(u32 i=0;i<nsec;i++){
    u64 s=sectbl+(u64)i*40, va=f32(s+12), span=f32(s+8), raw=f32(s+16), ch=f32(s+36);
    if(raw>span) span=raw;
    u64 lim=va+((span+0xfff)&~0xfffULL);
    if((ch&0x40000000u) && o>=va && o<lim && lim>limit) limit=lim;
  }
  return limit>o?(size_t)(limit-o):0;
}
static int read_exact(int fd,u8 *buf,size_t len){ size_t done=0; while(done<len){ ssize_t n=read(fd,buf+done,len-done); if(n<0){ if(errno==EINTR) continue; return -1; } if(!n) return -1; done+=(size_t)n; } return 0; }
static u32 f32(u64 o){ u32 v=0; if(file_range(o,4)) memcpy(&v,g_file+o,4); return v; }
static u64 f64(u64 o){ u64 v=0; if(file_range(o,8)) memcpy(&v,g_file+o,8); return v; }
static u16 f16(u64 o){ u16 v=0; if(file_range(o,2)) memcpy(&v,g_file+o,2); return v; }
static int pe_fail(const char *why){ fprintf(stderr,"[loader] invalid PE: %s\n",why); return -1; }

// ---------- fake TEB / gs ----------
static u8 g_teb[0x2000];
static u8 g_peb[0x1000];
static void *g_tls_array[64];
static u8  g_tls_block[0x2000];

static int set_gs(void *base){ return syscall(SYS_arch_prctl, ARCH_SET_GS, base); }

// ---------- import shim table ----------
typedef struct { const char *name; void *fn; } Shim;
static Shim shims[128]; static int nshims;
static void reg(const char*n, void*f){ if(nshims>=(int)(sizeof shims/sizeof*shims)){fprintf(stderr,"ft_engine: shim table full (%d) — raise shims[]\n",nshims);abort();} shims[nshims].name=n; shims[nshims].fn=f; nshims++; }
static void* find_shim(const char*n){ for(int i=0;i<nshims;i++) if(!strcmp(shims[i].name,n)) return shims[i].fn; return 0; }

// ================= SHIMS (all MS ABI) =================
static u32 g_lasterr=0;
static void MS sh_SetLastError(u32 e){ g_lasterr=e; }
static u32  MS sh_GetLastError(void){ return g_lasterr; }

static void* MS sh_GetProcessHeap(void){ return (void*)0x100; }
static void* MS sh_HeapAlloc(void*h,u32 flags,u64 size){ void*p=malloc(size?size:1); if(p&&(flags&8)) memset(p,0,size); return p; }
static int   MS sh_HeapFree(void*h,u32 f,void*p){ free(p); return 1; }
static void* MS sh_HeapReAlloc(void*h,u32 f,void*p,u64 s){ return realloc(p,s); }
static u64   MS sh_HeapSize(void*h,u32 f,void*p){ return 0; }
static int   MS sh_HeapValidate(void*h,u32 f,void*p){ return 1; }
static int   MS sh_HeapQueryInformation(void*h,int c,void*b,u64 l,u64*r){ return 0; }

static void MS sh_InitCS(void*p){ }
static u32  MS sh_InitCSSpin(void*p,u32 s){ return 1; }
static void MS sh_EnterCS(void*p){ }
static void MS sh_LeaveCS(void*p){ }
static void MS sh_DeleteCS(void*p){ }
static void MS sh_InitSList(void*p){ memset(p,0,16); }
static void* MS sh_FlushSList(void*p){ return 0; }

// TLS
static u32 g_tls_next=1; static void* g_tls_vals[1088];
static u32  MS sh_TlsAlloc(void){ return g_tls_next++; }
static int  MS sh_TlsFree(u32 i){ return 1; }
static void* MS sh_TlsGetValue(u32 i){ g_lasterr=0; return i<1088?g_tls_vals[i]:0; }
static int  MS sh_TlsSetValue(u32 i,void*v){ if(i<1088) g_tls_vals[i]=v; return 1; }

// process/thread/module
static void* MS sh_GetCurrentProcess(void){ return (void*)-1; }
static u32  MS sh_GetCurrentProcessId(void){ return 1234; }
static u32  MS sh_GetCurrentThreadId(void){ return 5678; }
static void* MS sh_GetModuleHandleW(void*n){ return (void*)g_image; }
static void* MS sh_GetModuleHandleA(void*n){ return (void*)g_image; }
static int  MS sh_GetModuleHandleExW(u32 f,void*n,void**h){ if(h)*h=(void*)g_image; return 1; }
static u32  MS sh_GetModuleFileNameA(void*m,char*b,u32 n){ strncpy(b,g_dllpath,n); return strlen(g_dllpath); }
static u32  MS sh_GetModuleFileNameW(void*m,u16*b,u32 n){ u32 i=0; for(;g_dllpath[i]&&i<n-1;i++) b[i]=g_dllpath[i]; b[i]=0; return i; }
static void* MS sh_LoadLibraryExW(void*n,void*h,u32 f){ return (void*)0x1000; }
static int  MS sh_FreeLibrary(void*h){ return 1; }
static void* MS sh_GetProcAddress(void*h,const char*n){ if((u64)n>>16==0) return 0; /*by ordinal*/ return find_shim(n); }

// console/file → route engine logs to stderr so we SEE its [Engine] output
static void* MS sh_GetStdHandle(u32 n){ return (void*)(u64)(n?n:1); }
static int  MS sh_SetStdHandle(u32 n,void*h){ return 1; }
static u32  MS sh_GetFileType(void*h){ return 2; /*CHAR*/ }
static int  MS sh_GetConsoleMode(void*h,u32*m){ if(m)*m=0; return 1; }
static u32  MS sh_GetConsoleCP(void){ return 437; }
static int  MS sh_WriteFile(void*h,const void*b,u32 n,u32*wr,void*o){ fwrite(b,1,n,stderr); if(wr)*wr=n; return 1; }
static int  MS sh_WriteConsoleW(void*h,const u16*b,u32 n,u32*wr,void*r){ for(u32 i=0;i<n;i++) fputc(b[i]&0xff,stderr); if(wr)*wr=n; return 1; }
static void MS sh_OutputDebugStringA(const char*s){ fprintf(stderr,"%s",s?s:""); }
static void MS sh_OutputDebugStringW(const u16*s){ if(s)for(;*s;s++) fputc(*s&0xff,stderr); }
static int  MS sh_FlushFileBuffers(void*h){ return 1; }
static void* MS sh_CreateFileW(void*n,u32 a,u32 s,void*sa,u32 c,u32 fl,void*t){ g_lasterr=2; return (void*)-1; } // config absent → graceful
static int  MS sh_SetFilePointerEx(void*h,long long d,void*np,u32 m){ return 0; }
static int  MS sh_CloseHandle(void*h){ return 1; }
static void* MS sh_FindFirstFileExA(void*n,int i,void*d,int o,void*s,u32 f){ return (void*)-1; }
static int  MS sh_FindNextFileA(void*h,void*d){ return 0; }
static int  MS sh_FindClose(void*h){ return 1; }
static u32  MS sh_GetSystemDirectoryA(char*b,u32 n){ strncpy(b,"C:\\Windows\\System32",n); return 19; }

// sync / events → the engine's global lock
static void* MS sh_CreateEventW(void*sa,int man,int init,void*name){ return (void*)0x2000; }
static int  MS sh_SetEvent(void*h){ return 1; }
static int  MS sh_ResetEvent(void*h){ return 1; }
static u32  MS sh_WaitForSingleObject(void*h,u32 ms){ return 0; /*WAIT_OBJECT_0*/ }
static u32  MS sh_WaitForSingleObjectEx(void*h,u32 ms,int a){ return 0; }
static void* MS sh_CreateThread(void*sa,u64 st,void*fn,void*arg,u32 fl,u32*id){ if(id)*id=4321; return (void*)0x3000; }
static int  MS sh_DeviceIoControl(void*h,u32 c,void*ib,u32 il,void*ob,u32 ol,u32*ret,void*ov){ return 0; }

// locale / codepage (ucrt init)
static u32  MS sh_GetACP(void){ return 1252; }
static u32  MS sh_GetOEMCP(void){ return 437; }
static int  MS sh_GetCPInfo(u32 cp,void*info){ if(info){ u8*p=info; *(u32*)p=1; p[4]=0; p[5]=0; } return 1; }
static int  MS sh_IsValidCodePage(u32 cp){ return 1; }
static int  MS sh_MultiByteToWideChar(u32 cp,u32 f,const char*mb,int mbc,u16*wc,int wcc){
  if(mbc<0) mbc=strlen(mb)+1;
  if(wcc==0) return mbc;
  int n=mbc<wcc?mbc:wcc;
  for(int i=0;i<n;i++) wc[i]=(u8)mb[i];
  return n; }
static int  MS sh_WideCharToMultiByte(u32 cp,u32 f,const u16*wc,int wcc,char*mb,int mbc,void*d,void*u){
  if(wcc<0){ wcc=0; while(wc[wcc]) wcc++; wcc++; } if(mbc==0) return wcc; int n=wcc<mbc?wcc:mbc; for(int i=0;i<n;i++) mb[i]=(char)wc[i]; return n; }
static int  MS sh_LCMapStringW(u32 l,u32 f,const u16*s,int sc,u16*d,int dc){ if(dc==0) return sc; int n=sc<dc?sc:dc; for(int i=0;i<n;i++) d[i]=s[i]; return n; }
static int  MS sh_GetStringTypeW(u32 t,const u16*s,int c,u16*out){ for(int i=0;i<c;i++) out[i]=0; return 1; }

// misc / version / cpu
static void MS sh_GetSystemInfo(void*p){ memset(p,0,64); u8*b=p; *(u32*)(b+0)=9; *(u32*)(b+4)=4096; }
static void MS sh_GetSystemTimeAsFileTime(u64*ft){ static u64 t=0x01d0000000000000ULL; *ft=(t+=100000); }
static u32  MS sh_GetTickCount(void){ static u32 t=1000; return (t+=5); }
static int  MS sh_QueryPerformanceCounter(u64*c){ static u64 t=0; *c=(t+=1000); return 1; }
static int  MS sh_IsDebuggerPresent(void){ return 0; }
static int  MS sh_IsProcessorFeaturePresent(u32 f){ return 1; }
static void* MS sh_EncodePointer(void*p){ return p; }
static void* MS sh_DecodePointer(void*p){ return p; }
static u64  MS sh_VerSetConditionMask(u64 m,u32 t,u8 c){ return m; }
static int  MS sh_VerifyVersionInfoA(void*vi,u32 tm,u64 cm){ return 1; }
static char g_cmd[]="loader"; static u16 g_cmdw[]={'l','o','a','d','e','r',0};
static char* MS sh_GetCommandLineA(void){ return g_cmd; }
static u16*  MS sh_GetCommandLineW(void){ return g_cmdw; }
static void MS sh_GetStartupInfoW(void*si){ memset(si,0,104); *(u32*)si=104; }
static u16 g_env[2]={0,0};
static u16* MS sh_GetEnvironmentStringsW(void){ return g_env; }
static int  MS sh_FreeEnvironmentStringsW(void*p){ return 1; }

// exception/unwind (only hit if the engine throws — hope not)
static void MS sh_RtlCaptureContext(void*ctx){ memset(ctx,0,1232); }
static void* MS sh_RtlLookupFunctionEntry(u64 pc,u64*base,void*hist){ if(base)*base=g_imagebase; return 0; }
static void* MS sh_RtlVirtualUnwind(u32 t,u64 b,u64 pc,void*fe,void*ctx,void**hd,u64*est,void*ctxp){ return 0; }
static void MS sh_RtlUnwindEx(void*a,void*b,void*c,void*d,void*e,void*f){ }
static void* MS sh_RtlPcToFileHeader(void*pc,void**base){ if(base)*base=(void*)g_image; return (void*)g_image; }
static void* MS sh_SetUnhandledExceptionFilter(void*f){ return 0; }
static u32  MS sh_UnhandledExceptionFilter(void*p){ return 1; }
static void MS sh_RaiseException(u32 code,u32 fl,u32 n,void*a){ fprintf(stderr,"[!] RaiseException code=%#x\n",code); }
static void MS sh_TerminateProcess(void*h,u32 c){ fprintf(stderr,"[!] TerminateProcess(%u)\n",c); }
static void MS sh_ExitProcess(u32 c){ fprintf(stderr,"[!] ExitProcess(%u)\n",c); }

static void register_shims(void){
#define R(n) reg(#n, (void*)sh_##n)
  R(SetLastError);R(GetLastError);R(GetProcessHeap);R(HeapAlloc);R(HeapFree);R(HeapReAlloc);
  R(HeapSize);R(HeapValidate);R(HeapQueryInformation);
  reg("InitializeCriticalSection",sh_InitCS);reg("InitializeCriticalSectionAndSpinCount",sh_InitCSSpin);
  reg("EnterCriticalSection",sh_EnterCS);reg("LeaveCriticalSection",sh_LeaveCS);reg("DeleteCriticalSection",sh_DeleteCS);
  reg("InitializeSListHead",sh_InitSList);reg("InterlockedFlushSList",sh_FlushSList);
  R(TlsAlloc);R(TlsFree);R(TlsGetValue);R(TlsSetValue);
  R(GetCurrentProcess);R(GetCurrentProcessId);R(GetCurrentThreadId);
  R(GetModuleHandleW);R(GetModuleHandleA);R(GetModuleHandleExW);R(GetModuleFileNameA);R(GetModuleFileNameW);
  R(LoadLibraryExW);R(FreeLibrary);R(GetProcAddress);
  R(GetStdHandle);R(SetStdHandle);R(GetFileType);R(GetConsoleMode);R(GetConsoleCP);
  R(WriteFile);R(WriteConsoleW);R(OutputDebugStringA);R(OutputDebugStringW);R(FlushFileBuffers);
  R(CreateFileW);R(SetFilePointerEx);R(CloseHandle);R(FindFirstFileExA);R(FindNextFileA);R(FindClose);R(GetSystemDirectoryA);
  R(CreateEventW);R(SetEvent);R(ResetEvent);R(WaitForSingleObject);R(WaitForSingleObjectEx);R(CreateThread);R(DeviceIoControl);
  R(GetACP);R(GetOEMCP);R(GetCPInfo);R(IsValidCodePage);R(MultiByteToWideChar);R(WideCharToMultiByte);R(LCMapStringW);R(GetStringTypeW);
  R(GetSystemInfo);R(GetSystemTimeAsFileTime);R(GetTickCount);R(QueryPerformanceCounter);R(IsDebuggerPresent);R(IsProcessorFeaturePresent);
  R(EncodePointer);R(DecodePointer);R(VerSetConditionMask);R(VerifyVersionInfoA);
  R(GetCommandLineA);R(GetCommandLineW);R(GetStartupInfoW);R(GetEnvironmentStringsW);R(FreeEnvironmentStringsW);
  R(RtlCaptureContext);R(RtlLookupFunctionEntry);R(RtlVirtualUnwind);R(RtlUnwindEx);R(RtlPcToFileHeader);
  R(SetUnhandledExceptionFilter);R(UnhandledExceptionFilter);R(RaiseException);R(TerminateProcess);R(ExitProcess);
#undef R
}

// ================= PE LOADER =================
static int load_pe(void){
  int fd=open(g_dllpath,O_RDONLY); if(fd<0){perror("open");return -1;}
  off_t flen=lseek(fd,0,SEEK_END);
  if(flen<0 || lseek(fd,0,SEEK_SET)<0 || (uint64_t)flen<0x40 || (uint64_t)flen>SIZE_MAX){ close(fd); return pe_fail("invalid/truncated file"); }
  g_filelen=(size_t)flen;
  g_file=malloc(g_filelen); if(!g_file){ close(fd); return -1; }
  int read_result=read_exact(fd,g_file,g_filelen); close(fd);
  if(read_result) return pe_fail("short file read");
  if(f16(0)!=0x5a4d) return pe_fail("DOS signature");

  u32 e=f32(0x3c);
  if(!file_range(e,24) || f32(e)!=0x00004550) return pe_fail("PE signature/header");
  u16 machine=f16(e+4), nsec=f16(e+6), optsize=f16(e+20);
  u64 opt=(u64)e+24;
  if(machine!=0x8664 || optsize<112 || !file_range(opt,optsize) || f16(opt)!=0x20b) return pe_fail("expected PE32+ x86-64");
  u32 sizeofimage=f32(opt+56), hdrsize=f32(opt+60);
  g_image_size=sizeofimage;
  g_imagebase=f64(opt+24);
  u32 entry=f32(opt+16), n_dirs=f32(opt+108);
  u64 sectbl=opt+optsize;
  if(!sizeofimage || sizeofimage>0x80000000u || !hdrsize || hdrsize>sizeofimage || hdrsize>g_filelen || !file_range(sectbl,(u64)nsec*40) || entry>=sizeofimage) return pe_fail("invalid image/header/section table size");
  for(u32 i=0;i<nsec;i++){
    u64 s=sectbl+(u64)i*40;
    u32 vaddr=f32(s+12),vsize=f32(s+8),rawsize=f32(s+16),rawptr=f32(s+20);
    u32 span=vsize>rawsize?vsize:rawsize;
    if(vaddr>sizeofimage || span>sizeofimage-vaddr || !file_range(rawptr,rawsize)) return pe_fail("section outside file or image");
  }

  if(n_dirs>16 || (u64)112+(u64)n_dirs*8>optsize) return pe_fail("data directories exceed optional header");

  /* The sidecar is laid out by RVA during installation. Mapping executable
   * pages from this read-only file avoids an executable memfd, which SELinux
   * rejects for fprintd. Imports live in non-executable private RW pages and
   * are resolved after mapping. */
  size_t imagepath_len=strlen(g_dllpath)+7;
  char *imagepath=malloc(imagepath_len);
  if(!imagepath) return -1;
  snprintf(imagepath,imagepath_len,"%s.image",g_dllpath);
  int imagefd=open(imagepath,O_RDONLY);
  if(imagefd<0){ perror("open engine image"); free(imagepath); return -1; }
  free(imagepath);
  struct stat image_stat;
  if(fstat(imagefd,&image_stat)<0 || image_stat.st_size!=(off_t)sizeofimage){ close(imagefd); return pe_fail("sidecar size does not match SizeOfImage"); }

  /* reserve the preferred base, then map each section from the file (W^X-safe) */
  void *m=mmap((void*)g_imagebase,sizeofimage,PROT_NONE,
               MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
  if(m==MAP_FAILED || m!=(void*)g_imagebase){
    fprintf(stderr,"[loader] reserve @%#lx failed (%p)\n",g_imagebase,m); close(imagefd); return -1;
  }
  g_image=m;
  u32 hdrmap=(hdrsize+0xfff)&~0xfffu;
  if(hdrmap>sizeofimage || mmap(g_image,hdrmap,PROT_READ,MAP_PRIVATE|MAP_FIXED,imagefd,0)==MAP_FAILED){
    perror("[loader] map headers"); close(imagefd); return -1;
  }
  for(int i=0;i<nsec;i++){
    u64 s=sectbl+(u64)i*40;
    u32 vaddr=f32(s+12), vsize=f32(s+8), rawsize=f32(s+16), chars=f32(s+36);
    u32 span=(vsize>rawsize?vsize:rawsize);
    u32 seglen=(span+0xfff)&~0xfffu;
    if(span>UINT32_MAX-0xfff || vaddr>sizeofimage || seglen>sizeofimage-vaddr){ close(imagefd); return pe_fail("section mapping range overflow"); }
    if(!seglen) continue;
    int prot=PROT_READ;
    if(chars&0x20000000) prot|=PROT_EXEC;   // IMAGE_SCN_MEM_EXECUTE
    else prot|=PROT_WRITE;                   // private data/IAT, never executable
    if(mmap(g_image+vaddr,seglen,prot,MAP_PRIVATE|MAP_FIXED,imagefd,vaddr)==MAP_FAILED){
      perror("[loader] map section"); close(imagefd); return -1;
    }
  }
  close(imagefd);

  /* Resolve kernel32 imports in the mapped, non-executable IAT. */
  u32 imprva=n_dirs>1?f32(opt+112+8):0, impsz=n_dirs>1?f32(opt+112+12):0;
  if(imprva && (impsz<20 || !image_range(imprva,impsz))){ return pe_fail("import directory outside image"); }
  u64 d=imprva;
  int descriptors_terminated=0;
  for(;imprva && d+20<=((u64)imprva+impsz);d+=20){
    if(!image_range(d,20)) return pe_fail("import descriptor outside image");
    u32 orig=*(u32*)(g_image+d), timestamp=*(u32*)(g_image+d+4), chain=*(u32*)(g_image+d+8), namer=*(u32*)(g_image+d+12), fthunk=*(u32*)(g_image+d+16);
    if(orig==0 && timestamp==0 && chain==0 && namer==0 && fthunk==0){ descriptors_terminated=1; break; }
    if(namer==0 || fthunk==0) return pe_fail("invalid import descriptor");
    if(!image_range(namer,1) || !image_range(fthunk,8) || (orig && !image_range(orig,8))) return pe_fail("import name/thunk outside image");
    /* IAT must be in mapped writable data, never code or a mapping gap. */
    if(!image_range(fthunk,8) || !image_writable_range(fthunk,8)) return pe_fail("IAT outside writable mapping");
    u64 rt=orig?orig:fthunk;
    int terminated=0;
    for(u32 j=0;image_range(rt+(u64)j*8,8) && image_range((u64)fthunk+(u64)j*8,8);j++){
      u64 ent=*(u64*)(g_image+rt+(u64)j*8);
      if(!ent){ terminated=1; break; }
      u64 *slot=(u64*)(g_image+(u64)fthunk+(u64)j*8);
      if(ent>>63){ *slot=0; continue; }
      u32 irva=(u32)(ent&0x7fffffff);
      if(!image_range(irva,3)) return pe_fail("import-by-name outside image");
      const char*fn=(char*)(g_image+irva+2);
      size_t name_limit=readable_range_length((u64)irva+2);
      if(!name_limit || !memchr(fn,0,name_limit)) return pe_fail("unterminated import name");
      void*sh=find_shim(fn);
      if(!sh) fprintf(stderr,"[loader] MISSING shim: %s\n",fn);
      *slot=(u64)sh;
    }
    if(!terminated) return pe_fail("unterminated import thunk array");
  }
  if(imprva && !descriptors_terminated) return pe_fail("unterminated import descriptor array");
  fprintf(stderr,"[loader] file-backed image at %#lx (no W+X), entry RVA %#x\n",g_imagebase,entry);

  // TLS setup (dir 9): copy template, wire teb->tls array
  u32 tlsrva=n_dirs>9?f32(opt+112+8*9):0;
  if(tlsrva){
    if(!image_range(tlsrva,40)) return pe_fail("TLS directory outside image");
    u64 start=*(u64*)(g_image+tlsrva);
    u64 end=*(u64*)(g_image+tlsrva+8);
    u64 idxaddr=*(u64*)(g_image+tlsrva+16);
    u64 cbaddr=*(u64*)(g_image+tlsrva+24);
    if(end<start || end-start>sizeof g_tls_block || (start && ((u8*)(uintptr_t)start<g_image || (u8*)(uintptr_t)start>=g_image+sizeofimage || end>(u64)(uintptr_t)(g_image+sizeofimage))) || (idxaddr && ((u8*)(uintptr_t)idxaddr<g_image || !image_range((u8*)(uintptr_t)idxaddr-g_image,4)))) return pe_fail("invalid TLS template/index range");
    u64 tlen=end-start;
    memset(g_tls_block,0,sizeof g_tls_block);
    if(tlen) memcpy(g_tls_block,(void*)start,tlen);
    g_tls_array[0]=g_tls_block;
    if(idxaddr) *(u32*)idxaddr=0;
    fprintf(stderr,"[loader] TLS template %lu bytes, callbacks@%#lx\n",tlen,cbaddr);
    // run TLS callbacks
    if(cbaddr){
      if((u8*)(uintptr_t)cbaddr<g_image || (u8*)(uintptr_t)cbaddr>=g_image+sizeofimage) return pe_fail("TLS callbacks outside image");
      u64 cb_rva=(u8*)(uintptr_t)cbaddr-g_image; int term=0;
      for(u32 i=0;image_range(cb_rva+(u64)i*8,8);i++){ u64 cb=*(u64*)(g_image+cb_rva+(u64)i*8); if(!cb){term=1;break;} if((u8*)(uintptr_t)cb<g_image || (u8*)(uintptr_t)cb>=g_image+sizeofimage) return pe_fail("TLS callback outside image"); void(MS *f)(void*,u32,void*)=(void*)cb; f((void*)g_imagebase,DLL_PROCESS_ATTACH,0); }
      if(!term) return pe_fail("unterminated TLS callbacks");
    }
  }

  // fake TEB in %gs
  memset(g_teb,0,sizeof g_teb); memset(g_peb,0,sizeof g_peb);
  *(u64*)(g_teb+0x30)=(u64)g_teb;            // NtTib.Self
  *(u64*)(g_teb+0x08)=(u64)(g_teb+0x2000);   // StackBase (dummy)
  *(u64*)(g_teb+0x10)=(u64)g_teb;            // StackLimit
  *(u64*)(g_teb+0x58)=(u64)g_tls_array;      // ThreadLocalStoragePointer
  *(u64*)(g_teb+0x60)=(u64)g_peb;            // PEB
  if(set_gs(g_teb)!=0){ perror("arch_prctl SET_GS"); return -1; }
  fprintf(stderr,"[loader] TEB installed in %%gs\n");

  // run entry point (DllMain via CRT startup)
  int(MS *DllMain)(void*,u32,void*)=(void*)(g_image+entry);
  fprintf(stderr,"[loader] calling entry (DllMain) ...\n");
  if(!image_range(entry,1)) return pe_fail("entry point outside image");
  int r=DllMain((void*)g_imagebase,DLL_PROCESS_ATTACH,0);
  fprintf(stderr,"[loader] DllMain returned %d\n",r);
  return r?0:-2;
}

// find an export by name
static void* get_export(const char*want){
  u32 e=f32(0x3c), opt=e+24;
  u32 n_dirs=f32(opt+108), exprva=n_dirs?f32(opt+112):0, expsz=n_dirs?f32(opt+116):0;
  if(!exprva || expsz<40 || !image_range(exprva,40)) return 0;
  u32 ordinal_base=*(u32*)(g_image+exprva+16), nfns=*(u32*)(g_image+exprva+20);
  u32 nnames=*(u32*)(g_image+exprva+24), fns=*(u32*)(g_image+exprva+28);
  u32 names=*(u32*)(g_image+exprva+32), ords=*(u32*)(g_image+exprva+36);
  if(!image_range(fns,(u64)nfns*4) || !image_range(names,(u64)nnames*4) || !image_range(ords,(u64)nnames*2)) return 0;
  for(u32 i=0;i<nnames;i++){
    u32 nrva=*(u32*)(g_image+(u64)names+(u64)i*4);
    if(!image_range(nrva,1)) return 0;
    const char *name=(char*)(g_image+nrva);
    size_t name_limit=readable_range_length(nrva);
    if(!name_limit || !memchr(name,0,name_limit)) return 0;
    if(!strcmp(name,want)){
      u16 ord=*(u16*)(g_image+(u64)ords+(u64)i*2);
      if(ord>=nfns || ordinal_base>UINT32_MAX-ord) return 0;
      u32 frva=*(u32*)(g_image+(u64)fns+(u64)ord*4);
      return image_range(frva,1)?g_image+frva:0;
    }
  }
  return 0;
}

// ================= engine driving (mirrors the Go harness) =================
static u8 pipeline[1024];
static u8 birbuf[262144];

static u8 small[262144];
static void resize(u8*src,int sw,int sh,int dw,int dh){ for(int y=0;y<dh;y++)for(int x=0;x<dw;x++) small[y*dw+x]=src[(y*sh/dh)*sw+(x*sw/dw)]; }
// Prepare an ENG_W x ENG_H frame in `small`. If the source is at least as large,
// crop the CENTERED window (preserves true ridge geometry — no aspect distortion);
// otherwise scale. FT9201_SCALE forces the old scale path for A/B testing.
#define FRAME_W 64
#define FRAME_H 80
static void prepare_frame(const u8*src,int sw,int sh){
  if(sw>=FRAME_W && sh>=FRAME_H && !getenv("FT9201_SCALE")){
    int ox=(sw-FRAME_W)/2, oy=(sh-FRAME_H)/2;
    for(int y=0;y<FRAME_H;y++) for(int x=0;x<FRAME_W;x++) small[y*FRAME_W+x]=src[(oy+y)*sw+(ox+x)];
  } else {
    resize((u8*)src, sw, sh, FRAME_W, FRAME_H);
  }
}
static int build_bir(u8*pix,int w,int h){
  int total=0x48+0x38+w*h; memset(birbuf,0,total);
  const int HDR=0x18,STD=0x48;
  *(u32*)(birbuf+0)=0x30; *(u32*)(birbuf+4)=HDR;
  *(u32*)(birbuf+8)=0x38+w*h; *(u32*)(birbuf+0xc)=STD;
  *(u16*)(birbuf+HDR+0x28)=0x001B; *(u16*)(birbuf+HDR+0x2a)=0x0401;
  *(u16*)(birbuf+STD+0x1c)=500; *(u16*)(birbuf+STD+0x1e)=500;
  birbuf[STD+0x22]=8; birbuf[STD+0x23]=0;
  *(u16*)(birbuf+STD+0x2c)=w; *(u16*)(birbuf+STD+0x2e)=h;
  memcpy(birbuf+STD+0x38,pix,w*h);
  return total;
}

// ================= STORAGE ADAPTER STUB (pipeline+0x28 vtable) =================
// Vtable: 0x20 header + function slots (standard WINBIO_STORAGE_INTERFACE order).
// Offsets the engine calls (verified by disassembly):
//   +0x68 AddRecord  +0x78 QueryBySubject  +0x90 FirstRecord
//   +0x98 NextRecord +0xa0 GetCurrentRecord
// WINBIO_STORAGE_RECORD fields the engine uses: +0x00 Identity*, +0x20 TemplateBlob,
//   +0x28 TemplateBlobSize.
static u8  g_rec_identity[76];
static u8  g_rec_template[1<<20];   // enrollment template can be ~200KB (up to 18 sub-tpls)
static u64 g_rec_tsize = 0;
static int g_have_record = 0;

static u64 MS st_default(void*a,void*b,void*c,void*d,void*e,void*f){ return 0; }

static u32 MS st_QueryBySubject(void*pipe,void*identity,u8 sub){
  fprintf(stderr,"[storage] QueryBySubject(sub=%u) -> %s\n", sub,
          g_have_record?"S_OK":"NO_RESULTS");
  if(identity) memcpy(g_rec_identity, identity, sizeof g_rec_identity);
  return g_have_record ? 0 : 0x8009801f; // WINBIO_E_DATABASE_NO_RESULTS
}
static u32 MS st_FirstRecord(void*pipe){
  fprintf(stderr,"[storage] FirstRecord -> %s\n", g_have_record?"S_OK":"NO_RESULTS");
  return g_have_record ? 0 : 0x8009801f;
}
static u32 MS st_NextRecord(void*pipe){
  fprintf(stderr,"[storage] NextRecord -> NO_MORE_RECORDS\n");
  return 0x80098020; // single-record store: always end after the first
}
static u32 MS st_GetCurrentRecord(void*pipe,u8*rec){
  fprintf(stderr,"[storage] GetCurrentRecord (tsize=%lu)\n",(unsigned long)g_rec_tsize);
  memset(rec,0,0x40);
  *(u64*)(rec+0x00)=(u64)g_rec_identity;
  *(u64*)(rec+0x20)=(u64)g_rec_template;
  *(u64*)(rec+0x28)=g_rec_tsize;
  return 0;
}
static u32 MS st_AddRecord(void*pipe,u8*rec,u8 sub){
  u64 tp=*(u64*)(rec+0x20), ts=*(u64*)(rec+0x28);
  u64 idp=*(u64*)(rec+0x00);
  fprintf(stderr,"[storage] AddRecord(sub=%u) template=%#lx size=%lu\n",sub,(unsigned long)tp,(unsigned long)ts);
  if(idp) memcpy(g_rec_identity,(void*)idp,sizeof g_rec_identity);
  if(tp && ts && ts<=sizeof g_rec_template){ memcpy(g_rec_template,(void*)tp,ts); g_rec_tsize=ts; g_have_record=1; }
  return 0;
}

static u64 g_storage_vt[0x100/8];
static u64 g_sensor_vt[0x100/8];
static void build_storage_vtable(void){
  for(int i=0;i<0x100/8;i++){ g_storage_vt[i]=(u64)st_default; g_sensor_vt[i]=(u64)st_default; }
  g_storage_vt[0x68/8]=(u64)st_AddRecord;
  g_storage_vt[0x78/8]=(u64)st_QueryBySubject;
  g_storage_vt[0x90/8]=(u64)st_FirstRecord;
  g_storage_vt[0x98/8]=(u64)st_NextRecord;
  g_storage_vt[0xa0/8]=(u64)st_GetCurrentRecord;
}


/* ==================== public API (ft_engine.h) ==================== */
#include "ft_engine.h"
#define ENG_W 64
#define ENG_H 80
static void *g_iface;

void ft_engine_geometry(int *w,int *h){ if(w)*w=ENG_W; if(h)*h=ENG_H; }

static int g_loaded = 0;
int ft_engine_open(const char *dll_path){
  /* Re-arm the %gs TEB on re-open: arch_prctl is per-thread and fprintd
   * re-opens the device (often on a different thread) between operations. */
  if(g_loaded){ set_gs(g_teb); return 0; }
  g_dllpath = dll_path ? dll_path : "ftWbioEngineAdapter.dll";
  register_shims();
  if(load_pe()!=0) return -1;
  void *q=get_export("WbioQueryEngineInterface");
  if(!q) return -2;
  u64 (MS *Query)(void**)=q;
  void *iface=0;
  if((u32)Query(&iface)!=0 || !iface) return -3;
  g_iface=iface;
  *(u64*)(pipeline+0x08)=~0ULL;
  build_storage_vtable();
  *(u64*)(pipeline+0x20)=(u64)g_sensor_vt;
  *(u64*)(pipeline+0x28)=(u64)g_storage_vt;
  u64 (MS *Attach)(void*)=*(void**)((u8*)iface+32+0*8);
  if((u32)Attach(pipeline)!=0) return -4;
  u64 ctx=*(u64*)(pipeline+0x38);
  *(u32*)(ctx+0x24)=ENG_W; *(u32*)(ctx+0x28)=ENG_H;
  g_loaded=1;
  return 0;
}

void ft_engine_close(void){
  /* The engine maps at a fixed base and is loaded once per process; a libfprint
   * device close/reopen must NOT tear it down (and must not clear g_iface, or the
   * next open — which short-circuits on the load-once guard — leaves a null
   * interface and the following engine call segfaults). Intentional no-op. */
}

uint32_t ft_engine_accept(const uint8_t *img,int sw,int sh,uint8_t purpose){
  set_gs(g_teb);   /* ensure %gs points at our TEB on this thread */
  prepare_frame((const u8*)img, sw, sh);
  int total=build_bir(small, ENG_W, ENG_H);
  u32 rej=0;
  u64 (MS *Accept)(void*,void*,u64,u64,void*)=*(void**)((u8*)g_iface+32+8*8);
  return (u32)Accept(pipeline, birbuf, total, purpose, &rej);
}

void ft_engine_enroll_begin(void){
  set_gs(g_teb);
  g_have_record=0; g_rec_tsize=0;
  u64 (MS *Create)(void*)=*(void**)((u8*)g_iface+32+12*8);
  Create(pipeline);
}

int ft_engine_enroll_update(void){
  set_gs(g_teb);
  u64 (MS *Update)(void*,void*)=*(void**)((u8*)g_iface+32+13*8);
  u64 (MS *Status)(void*,void*)=*(void**)((u8*)g_iface+32+14*8);
  u32 urej=0; u64 uhr=Update(pipeline,&urej);
  if((u32)uhr==0x80098008 || urej) return 2;   /* frame rejected (dup/low quality) */
  u32 srej=0; u64 shr=Status(pipeline,&srej);
  return (u32)shr==0 ? 0 : 1;                    /* 0 complete, 1 need more */
}

static void fill_identity(u8 *id){ memset(id,0,76); *(u32*)id=3; for(int i=0;i<16;i++) id[8+i]=0xA0+i; }

int ft_engine_enroll_commit(uint8_t **out,size_t *outlen){
  set_gs(g_teb);
  u8 id[76]; fill_identity(id);
  g_have_record=0;
  u64 (MS *Commit)(void*,void*,u8)=*(void**)((u8*)g_iface+32+17*8);
  if((u32)Commit(pipeline,id,1)!=0 || !g_have_record) return -1;
  *out=malloc(g_rec_tsize); if(!*out) return -2;
  memcpy(*out, g_rec_template, g_rec_tsize); *outlen=g_rec_tsize;
  return 0;
}

int ft_engine_verify(const uint8_t *tmpl,size_t tmpllen){
  set_gs(g_teb);
  if(tmpllen==0 || tmpllen>sizeof g_rec_template) return 0;
  memcpy(g_rec_template, tmpl, tmpllen); g_rec_tsize=tmpllen; g_have_record=1;
  u8 id[76]; fill_identity(id);
  u8 m=0; u32 ps=0,hs=0,vr=0; void*plp=0,*hp=0;
  u64 (MS *Verify)(void*,void*,u8,void*,void*,void*,void*,void*,void*)=*(void**)((u8*)g_iface+32+10*8);
  Verify(pipeline, id, 1, &m, &ps, &plp, &hs, &hp, &vr);
  return m?1:0;
}
