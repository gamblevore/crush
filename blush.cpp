// blush.cpp (original was crush.cpp)
// Written and placed in the public domain by Ilya Muravyov:         http://compressme.net
// A few optimisations and streaming-interface by Theodore H. Smith: http://gamblevore.org// OK so... here are some things I wanan do.

// * Figure out why its tighter than my original mz
	// 1) is it the 9-bit byte? seems a good idea
		// or is it how the length is encoded? or the offset?
	// 2) Can I put my 3-bit len back in? i'd hope so?
	// 3) how many bits does it take to get almost the same range? 16 still I think?
		// 4) can i still noob-encode offsets?
	// 5) is their hash-mmc better than my old suffix-array? (allow both to be used, do some kinda switch/option thingy)
		// 6) can I optimise my suffix-array with a stackless quicksort?
	// 7) can I make this a proper byte-aligned thing again? like store the escape bit somewhere...
		// 8) and maybe compress that too, using a range-coder. (the old bit-aware length-coder hahaha)
	// 9) Can I remove the chunk behaviour?
	// 10) Make a C-interface?
	// 11) decompress should share the buffer!! (do this first)
		// 12) get some proper timings? i think compress is 2x faster for mz, but what about decomp? 
	// 13) decomp buffer overruns with bad data? (why not comp too?)

// put this aside for now! its too many options!		 

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <chrono>

#if __cplusplus >= 201703L
	#define ifrare(b) if (b) [[unlikely]]
	#define ifok(b) if (b) [[likely]]
#else
	#define ifrare(b) if (b)
	#define ifok(b) if (b)
#endif


 
// CONFIG
static const int W_BITS=21; // Window size (17..23)
static const int BUF_SIZE=1<<24;
static const int HASH1_BITS=21;
static const int HASH2_BITS=24;

struct ByteSlice {
	typedef unsigned char u8;
	u8* Start;
	u8* Curr;
	u8* End;
	
	ByteSlice() {
		Start=0; Curr=0; End=0;
	}
	ByteSlice (int n) {
		Alloc(n, false);
	}
	ByteSlice (const char* Path) {
		Start=0;Curr=0;End=0;
		FILE* F = fopen(Path, "rb");
		if (F and !fseek(F, 0, SEEK_END)) {
			auto n = ftell(F);
			fseek(F, 0, SEEK_SET);
			Start = (u8*)malloc(n+1);
			if (Start) {
				Start[n] = 0;
				End = Start + n;
				Curr = Start;
				if (fread(Start, 1, n, F) != n)
					Free();
			}
		}
		if (F) fclose(F);
	}
	~ByteSlice () {
		Free();
	}
	
	bool Request (int Req) {
		return (Size() >= Req) or Alloc(Req, true);
	}

	bool Alloc (int n, bool Realloc) {
		if (Realloc and Start) {
			u8* Start2 = (u8*)realloc(Start, n);
			if (!Start2)
				return false;
			Start = Start2;
		} else {
			Start = (u8*)malloc(n);
		}
		
		Curr = Start;
		End = Curr+n*(Curr!=0);
		return (Curr!=0);
	}
	void Free () {
		free(Start);
		Start = 0; Curr = 0; End = 0;
	}
	uint32_t Read4() {
		auto C = (uint32_t*)Curr;
		auto R = *C++;
		Curr = (u8*)C;
		return R;
	}
	bool Write4(uint32_t X) {
		auto C = Curr;
		ifok (C < End) {
			*((int*)C) = X;
			Curr = C + 4;
			return true;
		}
		return false;
	}
	int Write(const u8* Data, int N) {
		int N2 = (int)(End - Curr); if (N2 > N) N2 = N;
		auto C = Curr; Curr = C + N2;
		memcpy(C, Data, N2);
		return N-N2;
	}
	bool HasMore () {
		return Curr < End;
	}
	int Remain () {
		return (int)(End - Curr);
	}
	int Size () {
		return (int)(End - Start);
	}
	int Used () {
		return (int)(Curr - Start);
	}
};

ByteSlice CompSpace; // can be freed.


struct CompressingBuffer {
	ByteSlice	Buff;
	FILE*		File;
	
	uint64_t	BitBuff;
	int			BitCount;

// C++ sucks. I'm sorry. I hate writing this code. Im so sorry. 😭
	CompressingBuffer(FILE* f, int n) : File(f), Buff(n), BitBuff(0), BitCount(0) {}
	CompressingBuffer(FILE* f) : File(f), Buff(), BitBuff(0), BitCount(0) {}
	
	unsigned char* OutBuffer(int StartAt, int Req) {
		if (!Buff.Request(Req))
			return 0;
		auto S = Buff.Start;
		Buff.Curr = S + StartAt;
		return S;
	}

	bool Flush() {
		if (Write(0,0)) return false;
		return true;
	}
	bool Write4(uint32_t bit32) {
		ByteSlice& S = Buff;
		ifok (S.Write4(bit32))							// all written
			return true;
		
		auto W = fwrite(S.Start, 1, S.Used(), File);
		if (W < S.Used()) return false;					// fail
		S.Curr = S.Start;
		return true;									// succeed
	}
	int Write(const void* pData, int Length) {
		auto Data = (const unsigned char*)pData;
		ByteSlice& S = Buff;
			
		int Remain = S.Write(Data, Length);
		ifok (!Remain and Length)						// all written
			return Length;
		
		Data += Length - Remain;						// its full
		auto Start = S.Start;
		while (true) {
			int ToWrite=(int)(S.Curr-Start);
			if (ToWrite <= 0) break;
			int Written = (int)fwrite(Start, 1, ToWrite, File);
			if (Written < 1) return -1;					// fail
			Start += Written;
		}
		
		S.Curr = S.Start;								// reset
		S.Write(Data, Remain);
		return Length;
	}
};



// Bit I/O

#define put_bits(n, x)   (put_bitss(n, x, Out))
#define get_bits(n)      (get_bitss(In, n, Out))
inline void put_bitss (int n, uint64_t x, CompressingBuffer& Out) {
	n += Out.BitCount;
	auto B = Out.BitBuff | (x<<(64-n));
	if (n >= 32) {
		Out.BitCount = n - 32;
		Out.BitBuff = B << 32;
		Out.Write4(B>>32);
	} else {
		Out.BitBuff = B;
		Out.BitCount = n;
	}
}

inline int get_bitss (ByteSlice& BS, int n, CompressingBuffer& Out) {
 	int C = Out.BitCount;
	uint64_t B = Out.BitBuff;
	if (C < n) {
		auto R = ((uint64_t)BS.Read4())<<(32-C);
		B |= R;
		C += 32;
	}

	int X = (int)(B>>(64-n));
	Out.BitBuff  = B<<n;
	Out.BitCount = C-n;
	return X;
}


// STUFF
static const int W_SIZE   =1<<W_BITS;
static const int W_MASK   =W_SIZE-1;
static const int SLOT_BITS=4;
static const int NUM_SLOTS=1<<SLOT_BITS;
static const int W_MINUS  =W_BITS-NUM_SLOTS;
static const int W_MINUS1 =W_MINUS+1;

static const int A_BITS=2; // 1 xx
static const int B_BITS=2; // 01 xx
static const int C_BITS=2; // 001 xx
static const int D_BITS=3; // 0001 xxx
static const int E_BITS=5; // 00001 xxxxx
static const int F_BITS=9; // 00000 xxxxxxxxx
static const int A=1<<A_BITS;
static const int B=(1<<B_BITS)+A;
static const int C=(1<<C_BITS)+B;
static const int D=(1<<D_BITS)+C;
static const int E=(1<<E_BITS)+D;
static const int F=(1<<F_BITS)+E;
static const int MIN_MATCH=3;
static const int MAX_MATCH=(F-1)+MIN_MATCH;



static const int TOO_FAR=1<<16;

static const int HASH1_LEN=MIN_MATCH;
static const int HASH2_LEN=MIN_MATCH+1;
static const int HASH1_SIZE=1<<HASH1_BITS;
static const int HASH2_SIZE=1<<HASH2_BITS;
static const int HASH1_MASK=HASH1_SIZE-1;
static const int HASH2_MASK=HASH2_SIZE-1;
static const int HASH1_SHIFT=(HASH1_BITS+(HASH1_LEN-1))/HASH1_LEN;
static const int HASH2_SHIFT=(HASH2_BITS+(HASH2_LEN-1))/HASH2_LEN;

inline int update_hash1(int h, int c) {
	return ((h<<HASH1_SHIFT)+c)&HASH1_MASK;
}

inline int update_hash2(int h, int c) {
	return ((h<<HASH2_SHIFT)+c)&HASH2_MASK;
}

inline int get_min(int a, int b) { return a<b?a:b; }
inline int get_max(int a, int b) { return a>b?a:b; }

inline int get_penalty(int a, int b) {
	int p=0;
	while (a>b) {
		a>>=3;
		++p;
	}
	return p;
}


bool compress (ByteSlice& In, int level, CompressingBuffer& Out) {
	const int Req = (HASH1_SIZE+HASH2_SIZE+W_SIZE)*sizeof(int);
	if (!CompSpace.Request(Req))
		return false;
	int* head = (int*)CompSpace.Start;
	int* prev = head + HASH1_SIZE+HASH2_SIZE;

	const int max_chain[]={4, 256, 1<<12};
	while (In.HasMore()) {
		int size = get_min(In.Remain(), BUF_SIZE);
		auto buf = In.Curr;
		In.Curr += size;
		Out.Write4(size);
		memset(head, -1, (HASH1_SIZE+HASH2_SIZE)*sizeof(int));
		int h1=0;
		int h2=0;
		for (int i=0; i<HASH1_LEN; ++i)
			h1=update_hash1(h1, buf[i]);
		for (int i=0; i<HASH2_LEN; ++i)
			h2=update_hash2(h2, buf[i]);

		
		for (int p=0;p<size;) {
			int len=MIN_MATCH-1;
			int offset=W_SIZE;

			const int max_match=get_min(MAX_MATCH, size-p);
			const int limit=get_max(p-W_SIZE, 0);

	
			/// FIND MATCH
			if (head[h1]>=limit) {
				int s=head[h1];
				if (buf[s]==buf[p]) {
					int l=0;
					while (++l<max_match)
						if (buf[s+l]!=buf[p+l])
							break;
					if (l>len) {
						len=l;
						offset=p-s;
					}
				}
			}

			if (len<MAX_MATCH) {
				int chain_len=max_chain[level];
				int s=head[h2+HASH1_SIZE];

				while ((chain_len--!=0)&&(s>=limit)) {
					if ((buf[s+len]==buf[p+len])&&(buf[s]==buf[p])) {	
						int l=0;
						while (++l<max_match)
							if (buf[s+l]!=buf[p+l])
								break;
						if (l>len+get_penalty((p-s)>>4, offset)) {
							len=l;
							offset=p-s;
						}
						if (l==max_match)
							break;
					}
					s=prev[s&W_MASK];
				}
			}

			if ((len==MIN_MATCH)&&(offset>TOO_FAR))
				len=0;

			if ((level>=2)&&(len>=MIN_MATCH)&&(len<max_match)) {
				const int next_p=p+1;
				const int max_lazy=get_min(len+4, max_match);

				int chain_len=max_chain[level];
				int s=head[update_hash2(h2, buf[next_p+(HASH2_LEN-1)])+HASH1_SIZE];

				while ((chain_len--!=0)&&(s>=limit)) {
					if ((buf[s+len]==buf[next_p+len])&&(buf[s]==buf[next_p])) {
						int l=0;
						while (++l<max_lazy)
							if (buf[s+l]!=buf[next_p+l])
								break;
						if (l>len+get_penalty(next_p-s, offset)) {
							len=0;
							break;
						}
						if (l==max_lazy)
							break;
					}
					s=prev[s&W_MASK];
				}
			}
			/// END FIND MATCH


			if (len>=MIN_MATCH) { // Match. 14 bits for smallest item, of offset < 64 and length 1-4 (+minlength)
				put_bits(1, 1);	  // not sure why this beats my encoder as mine has longer offsets and good length?
								  // is it simply the byte-escaper?
				const int l=len-MIN_MATCH;
				if (l<A) {									// 1 	// 14 bits
					put_bits(A_BITS+1, l|(1<<A_BITS));
				} else if (l<B) {							// 01	// 15 bits
					put_bits(B_BITS+2, (l-A)|(1<<B_BITS));
				} else if (l<C) {							// 001	// 16 bits...
					put_bits(C_BITS+3, (l-B)|(1<<C_BITS));
				} else if (l<D) {							// 0001
					put_bits(D_BITS+4, (l-C)|(1<<D_BITS));
				} else if (l<E) {							// 00001
					put_bits(E_BITS+5, (l-D)|(1<<E_BITS));
				} else {									// 00000
					put_bits(F_BITS+5, (l-E)|(0<<E_BITS));
				}

				--offset;
				int log=W_MINUS;
				while (offset>=(2<<log))
					++log;
				put_bits(SLOT_BITS, log-W_MINUS);
				if (log>W_MINUS)
					put_bits(log, offset-(1<<log));
				  else
					put_bits(W_MINUS1, offset);
			} else { // Literal
				put_bits(9, buf[p]); // 0 xxxxxxxx
				len=1;
			}

			while (len--!=0) { // Insert new strings
				head[h1]=p;
				prev[p&W_MASK]=head[h2+HASH1_SIZE];
				head[h2+HASH1_SIZE]=p++;
				h1=update_hash1(h1, buf[p+(HASH1_LEN-1)]);
				h2=update_hash2(h2, buf[p+(HASH2_LEN-1)]);
			}
		}

		put_bits(31, 0);
		Out.BitCount=Out.BitBuff=0;
	}
	return Out.Flush();
}


int decompress (ByteSlice& In, CompressingBuffer& Out) {
	int Total = 0; // what about when we remove the chunk system? remove this too?
	while (In.Remain()) {
		int Size = In.Read4();
		Total += Size;
		ifrare (Size<1 or Size>BUF_SIZE)	return -2;
		ByteSlice::u8* buf = Out.OutBuffer(Size, Size+7);
		ifrare (!buf)						return -3;

		Out.BitCount = Out.BitBuff = 0;
		int p = 0;
		while (p<Size) {
			if (get_bits(1)) {
				int len;
				if (get_bits(1))
					len=get_bits(A_BITS)+MIN_MATCH;
				  else if (get_bits(1))
					len=get_bits(B_BITS)+A+MIN_MATCH;
				  else if (get_bits(1))
					len=get_bits(C_BITS)+B+MIN_MATCH;
				  else if (get_bits(1))
					len=get_bits(D_BITS)+C+MIN_MATCH;
				  else if (get_bits(1))
					len=get_bits(E_BITS)+D+MIN_MATCH;
				  else
					len=get_bits(F_BITS)+E+MIN_MATCH;

				const int log=get_bits(SLOT_BITS) + W_MINUS;
				int ago;
				if (log>W_MINUS)
					ago = get_bits(log)+(1<<log);
				  else
					ago = get_bits(W_MINUS1);
				int s = p - (ago+1);
				ifrare (s < 0)				return -4;
				ifrare (len>Size-p) // buffer overrun. Just save as much as we can.
					len = Size-p;
				
				if (ago>=8) { // much faster :)
					int L8 = (len + 7) >> 3;
					auto W = (uint64_t*)(buf+p);
					auto R = (uint64_t*)(buf+s);
					while (L8--)
						*W++ = *R++;
					p+=len;
				} else while (len--)
					buf[p++] = buf[s++];
			} else {
				buf[p++] = get_bits(8);
			}
		}
		ifrare (!Out.Flush())				return -1;
	}

	return Total;
}



int main(int argc, char* argv[]) {
	auto StartTime = std::chrono::system_clock::now();

	if (argc!=4) {
		fprintf(stdout,
			"Original CRUSH by Ilya Muravyov\n"
			"Optimisations by Theodore H. Smith\n"
			"Usage: blush command infile outfile\n"
			"Commands:\n"
			"  c[f,x] Compress (Fast..Max)\n"
			"  d      Decompress\n");
		exit(1);
	}

	auto In = ByteSlice(argv[2]);
	if (!In.HasMore()) {
		perror(argv[2]);
		exit(1);
	}
	
	FILE* f = fopen(argv[3], "wb");
	if (!f) {
		perror(argv[3]);
		exit(1);
	}
	
	int A = In.Size();
	if (*argv[1]=='c') {
		printf("Compressing %s...\n", argv[2]);
		auto c = argv[1][1];
		int level = 1 + (c=='x') - (c=='f');
		CompressingBuffer Out(f, 8*1024*1024); 
		compress(In, level, Out);

	} else if (*argv[1]=='d') {
		printf("Decompressing %s...\n", argv[2]);
		CompressingBuffer Out(f);
		int Err = decompress(In, Out);
		if (Err<0) {
			fprintf(stderr, "Decompression failed: %i\n", Err);
			exit(1);
		}
	
	} else {
		fprintf(stderr, "Unknown command: %s\n", argv[1]);
		exit(1);
	}

	auto EndTime = std::chrono::system_clock::now();
	auto Durr    = std::chrono::duration<double>(EndTime-StartTime);
	auto Seconds = Durr.count();
	int  B       = (int)ftell(f);
	auto Ratio   = (100.0*(double)B)/(double)A;
	int  XX		 = (*argv[1]=='c') ? A:B; 
	auto MBPS	 = ((double)XX/(1024.0*1024.0)) / Seconds;
	printf("%i -> %i in %.2fs (%.1f%%) at %.2fMB/s\n", A, B,
		Seconds, Ratio, MBPS);

	fclose(f);

	return 0;
}

// Note: Crush's length coder seems really tight. I could use this outside of crush? 
