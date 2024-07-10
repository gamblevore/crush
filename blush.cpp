// blush.cpp (original was crush.cpp)
// Written and placed in the public domain by Ilya Muravyov:         http://compressme.net
// A few optimisations and streaming-interface by Theodore H. Smith: http://gamblevore.org

// compile: g++ -O3 blush.cpp -o blush -std=c++17
// todo: make a proper C interface, 
		 

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

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
		Alloc(n);
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
	
	bool Alloc (int n) {
		Curr = Start = (u8*)malloc(n);
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
		if (C < End) [[likely]] {
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

	CompressingBuffer(FILE* f, int n=1024*1024*8) : File(f), Buff(n) {}
	
	bool Flush() {
		if (Write(0,0)) return false;
		return true;
	}
	bool Write4(uint32_t bit32) {
		ByteSlice& S = Buff;
		if (S.Write4(bit32)) [[likely]]					// all written
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
		if (!Remain and Length) [[likely]]				// all written
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
#define get_bits(n)      (get_bitss(S, n, Out))
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


bool compress (ByteSlice& Src, int level, CompressingBuffer& Out) {
	if (!CompSpace.Start) {
		int Req = (HASH1_SIZE+HASH2_SIZE+W_SIZE)*sizeof(int);
		if (!CompSpace.Alloc(Req))
			return false;
	}
	int* head = (int*)CompSpace.Start;
	int* prev = head + HASH1_SIZE+HASH2_SIZE;

	const int max_chain[]={4, 256, 1<<12};
	while (Src.HasMore()) {
		int size = get_min(Src.Remain(), BUF_SIZE);
		auto buf = Src.Curr;
		Src.Curr += size;
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

			if (len>=MIN_MATCH) { // Match
				put_bits(1, 1); // could even merge this :3

				const int l=len-MIN_MATCH;
				if (l<A) {									// 1 
					put_bits(A_BITS+1, l|(1<<A_BITS));
				} else if (l<B) {							// 01
					put_bits(B_BITS+2, (l-A)|(1<<B_BITS));
				} else if (l<C) {							// 001
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


bool decompress (ByteSlice& S, CompressingBuffer& Out, int* Err) {
	static unsigned char buf[BUF_SIZE+MAX_MATCH+7];

	while (S.Remain()) {
		int size = S.Read4();
		if (size<1 or size>BUF_SIZE) {
			*Err = size;
			return false;
		}

		Out.BitCount = Out.BitBuff = 0;
		int p = 0;
		while (p<size) {
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
				if (s < 0) {
					*Err = s;
					return false;
				}
				
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
		Out.Write(buf, p);
	}

	return Out.Flush();
}



int main(int argc, char* argv[]) {
	const clock_t start=clock();

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
	
	CompressingBuffer Out = fopen(argv[3], "wb");
	if (!Out.File) {
		perror(argv[3]);
		exit(1);
	}
	
	int A = In.Size();
	if (*argv[1]=='c') {
		printf("Compressing %s...\n", argv[2]);
		int level = argv[1][1]=='f' ? 0:(argv[1][1]=='x' ? 2:1);
		compress(In, level, Out);

	} else if (*argv[1]=='d') {
		printf("Decompressing %s...\n", argv[2]);
		int Err = 0;
		if (!decompress(In, Out, &Err)) {
			fprintf(stderr, "Decompression failed: %i\n", Err);
			exit(1);
		}
			
	
	} else {
		fprintf(stderr, "Unknown command: %s\n", argv[1]);
		exit(1);
	}

	int B = (int)ftell(Out.File);
	printf("%i -> %i in %.2fs (%.2f%%)\n", A, B,
		double(clock()-start)/CLOCKS_PER_SEC, (100.0*(double)B)/(double)A);

	fclose(Out.File);

	return 0;
}

// Note: Crush's length coder seems really tight. I could use this outside of crush? 
