/**
 * @author  Adham Ehab   @date 18/08/2026
 *
 * TweetNaCl requires an external randombytes() symbol. Signature *verification*
 * never calls it, so this stub only exists to satisfy the linker. Do NOT use
 * this device to generate keys - it has no real entropy source.
 */
void randombytes(unsigned char *p, unsigned long long n)
{
    (void)p;
    (void)n;
}
