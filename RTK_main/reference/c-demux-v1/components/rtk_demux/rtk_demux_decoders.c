// 通用 GNSS 协议解码器描述符（ADR-0004 §4）。纯 C11，无平台/IDF 依赖，ISR 可调用。
// - 每个描述符都是 const 对象，可被多个 rtk_demux_t 实例共享；step 是 frame[0..len) 的纯函数，
//   不写任何静态可变状态，可对同一前缀重复调用（分流器的回放重扫依赖这一点）。
// - 长度字段已知后一次返回整段 need，让分流器整块 memcpy、只再调用一次 step；校验在帧尾整段计算。
// - 长度未知的 ASCII 协议（NMEA、Unicore Hash）每次 need = 1，且只检查新到字节及其前面定长的
//   尾部窗口（NMEA 5 B、Hash 11 B，O(1)）；遇到 '\n' 时才整段复核正文与校验值。
// - CRC-24Q 与 NovAtel/Unicore 风格 CRC-32 用 256 项 const 查表（各 1 KB Flash）。两张表由独立的
//   逐位 Python 实现生成并交叉核对：test/demux_dev/g2/gen_vectors.py 与 docs/reports/G2.md。
// 线程安全：是（只读 const 表与只读入参）；ISR 安全：是（无阻塞、无分配、无浮点）。
//
// Generated-by: pi + DeepSeek-V4.1-Flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "rtk_demux_decoders.h"

/* ---------------------------------------------------------------- 常量 */

#define RTK_DEMUX_PRIV_BYTE_BITS   (8U)
#define RTK_DEMUX_PRIV_U16_BYTES   (2U)
#define RTK_DEMUX_PRIV_U24_BYTES   (3U)
#define RTK_DEMUX_PRIV_U32_BYTES   (4U)
#define RTK_DEMUX_PRIV_BYTE_MASK   (0xFFUL)
#define RTK_DEMUX_PRIV_CRC24Q_MASK (0xFFFFFFUL)

/* 各协议的 max_frame（描述符与 step 的长度上限共用，避免两处不一致）。 */
#define RTK_DEMUX_PRIV_RTCM3_MAX_FRAME        (1029U)
#define RTK_DEMUX_PRIV_NMEA_MAX_FRAME         (256U)
#define RTK_DEMUX_PRIV_UBX_MAX_FRAME          (4096U)
#define RTK_DEMUX_PRIV_UNICORE_BIN_MAX_FRAME  (4096U)
#define RTK_DEMUX_PRIV_UNICORE_HASH_MAX_FRAME (1024U)

/* 同步头与固定头长度。 */
#define RTK_DEMUX_PRIV_SYNC_RTCM3     (0xD3U)
#define RTK_DEMUX_PRIV_SYNC_NMEA      (0x24U) /* '$' */
#define RTK_DEMUX_PRIV_SYNC_UBX_A     (0xB5U)
#define RTK_DEMUX_PRIV_SYNC_UBX_B     (0x62U)
#define RTK_DEMUX_PRIV_SYNC_UNICORE_A (0xAAU)
#define RTK_DEMUX_PRIV_SYNC_UNICORE_B (0x44U)
#define RTK_DEMUX_PRIV_SYNC_UNICORE_C (0xB5U)
#define RTK_DEMUX_PRIV_SYNC_HASH      (0x23U) /* '#' */
#define RTK_DEMUX_PRIV_CR             (0x0DU)
#define RTK_DEMUX_PRIV_LF             (0x0AU)
#define RTK_DEMUX_PRIV_STAR           (0x2AU) /* '*' */
#define RTK_DEMUX_PRIV_PRINTABLE_MIN  (0x20U)
#define RTK_DEMUX_PRIV_PRINTABLE_MAX  (0x7EU)

/* RTCM3：3 B 头 + 载荷 + 3 B CRC-24Q；第 2 字节高 6 位保留，低 2 位是长度高 2 位。 */
#define RTK_DEMUX_PRIV_RTCM3_HDR_LEN       (3U)
#define RTK_DEMUX_PRIV_RTCM3_CRC_LEN       (3U)
#define RTK_DEMUX_PRIV_RTCM3_RESERVED_MASK (0xFCU)
#define RTK_DEMUX_PRIV_RTCM3_LEN_HI_MASK   (0x03U)

/* UBX：B5 62 | class | id | len(2, LE) | 载荷 | CK_A CK_B。 */
#define RTK_DEMUX_PRIV_UBX_SYNC_LEN (2U)
#define RTK_DEMUX_PRIV_UBX_HDR_LEN  (6U)
#define RTK_DEMUX_PRIV_UBX_CK_LEN   (2U)
#define RTK_DEMUX_PRIV_UBX_OFS_LEN  (4U)

/* Unicore 二进制：24 B 固定头（载荷长在偏移 6，u16 LE）+ 载荷 + 4 B CRC-32（LE）。 */
#define RTK_DEMUX_PRIV_UCBIN_HDR_LEN (24U)
#define RTK_DEMUX_PRIV_UCBIN_CRC_LEN (4U)
#define RTK_DEMUX_PRIV_UCBIN_OFS_LEN (6U)

/* 尾部窗口长度：find_star_reverse 的搜索范围，固定因此 O(1)。
 * NMEA "*hh\r\n" 共 5 B；Unicore Hash "*hhhhhhhh\r\n" 共 11 B。 */
#define RTK_DEMUX_PRIV_NMEA_STAR_WINDOW (4U)
#define RTK_DEMUX_PRIV_HASH_STAR_WINDOW (10U)

/* NMEA 尾部以 '\n' 为参照的固定偏移：'\n' 0 | '\r' 1 | 低位十六进制 2 | 高位十六进制 3 | '*' 4。 */
#define RTK_DEMUX_PRIV_NMEA_OFS_STAR   (4U)
#define RTK_DEMUX_PRIV_NMEA_OFS_HEX_HI (3U)
#define RTK_DEMUX_PRIV_NMEA_MIN_LEN    (7U) /* "$" + 发送方标识 + "*hh\r\n" */
#define RTK_DEMUX_PRIV_NIBBLE_BITS     (4U)
#define RTK_DEMUX_PRIV_HEX_ALPHA_BASE  (10U) /* 'A' / 'a' 对应的数值 */

/* Unicore Hash 尾部两种形式以 '\n' 为参照的 '*' 偏移（由近及远搜索，先命中较短的 XOR 形式）。 */
#define RTK_DEMUX_PRIV_HASH_OFS_STAR_XOR (4U)  /* "*hh\r\n" */
#define RTK_DEMUX_PRIV_HASH_OFS_STAR_CRC (10U) /* "*hhhhhhhh\r\n" */
#define RTK_DEMUX_PRIV_HASH_CRC_DIGITS   (8U)
#define RTK_DEMUX_PRIV_XOR_DIGITS        (2U)
#define RTK_DEMUX_PRIV_HASH_OFS_BRANCH   (3U) /* '*' 后第 3 字节：XOR 形式为 '\r'，CRC 形式仍为十六进制 */
#define RTK_DEMUX_PRIV_HASH_OFS_CR       (9U) /* '*' 后第 9 字节：CRC 形式的 '\r' */

/* ---------------------------------------------------------------- CRC 查表 */

/* CRC-24Q（RTCM 10403.3 §3.3；= CRC-24/LTE-A）：poly 0x1864CFB，init 0，不反射，不取反。
 * 表项 = 单字节 8 次移位后的余数，值域 < 2^24。生成方式见本文件头注释。 */
static const uint32_t s_crc24q_table[256] = {
    0x000000UL, 0x864CFBUL, 0x8AD50DUL, 0x0C99F6UL, 0x93E6E1UL, 0x15AA1AUL, 0x1933ECUL, 0x9F7F17UL, 0xA18139UL,
    0x27CDC2UL, 0x2B5434UL, 0xAD18CFUL, 0x3267D8UL, 0xB42B23UL, 0xB8B2D5UL, 0x3EFE2EUL, 0xC54E89UL, 0x430272UL,
    0x4F9B84UL, 0xC9D77FUL, 0x56A868UL, 0xD0E493UL, 0xDC7D65UL, 0x5A319EUL, 0x64CFB0UL, 0xE2834BUL, 0xEE1ABDUL,
    0x685646UL, 0xF72951UL, 0x7165AAUL, 0x7DFC5CUL, 0xFBB0A7UL, 0x0CD1E9UL, 0x8A9D12UL, 0x8604E4UL, 0x00481FUL,
    0x9F3708UL, 0x197BF3UL, 0x15E205UL, 0x93AEFEUL, 0xAD50D0UL, 0x2B1C2BUL, 0x2785DDUL, 0xA1C926UL, 0x3EB631UL,
    0xB8FACAUL, 0xB4633CUL, 0x322FC7UL, 0xC99F60UL, 0x4FD39BUL, 0x434A6DUL, 0xC50696UL, 0x5A7981UL, 0xDC357AUL,
    0xD0AC8CUL, 0x56E077UL, 0x681E59UL, 0xEE52A2UL, 0xE2CB54UL, 0x6487AFUL, 0xFBF8B8UL, 0x7DB443UL, 0x712DB5UL,
    0xF7614EUL, 0x19A3D2UL, 0x9FEF29UL, 0x9376DFUL, 0x153A24UL, 0x8A4533UL, 0x0C09C8UL, 0x00903EUL, 0x86DCC5UL,
    0xB822EBUL, 0x3E6E10UL, 0x32F7E6UL, 0xB4BB1DUL, 0x2BC40AUL, 0xAD88F1UL, 0xA11107UL, 0x275DFCUL, 0xDCED5BUL,
    0x5AA1A0UL, 0x563856UL, 0xD074ADUL, 0x4F0BBAUL, 0xC94741UL, 0xC5DEB7UL, 0x43924CUL, 0x7D6C62UL, 0xFB2099UL,
    0xF7B96FUL, 0x71F594UL, 0xEE8A83UL, 0x68C678UL, 0x645F8EUL, 0xE21375UL, 0x15723BUL, 0x933EC0UL, 0x9FA736UL,
    0x19EBCDUL, 0x8694DAUL, 0x00D821UL, 0x0C41D7UL, 0x8A0D2CUL, 0xB4F302UL, 0x32BFF9UL, 0x3E260FUL, 0xB86AF4UL,
    0x2715E3UL, 0xA15918UL, 0xADC0EEUL, 0x2B8C15UL, 0xD03CB2UL, 0x567049UL, 0x5AE9BFUL, 0xDCA544UL, 0x43DA53UL,
    0xC596A8UL, 0xC90F5EUL, 0x4F43A5UL, 0x71BD8BUL, 0xF7F170UL, 0xFB6886UL, 0x7D247DUL, 0xE25B6AUL, 0x641791UL,
    0x688E67UL, 0xEEC29CUL, 0x3347A4UL, 0xB50B5FUL, 0xB992A9UL, 0x3FDE52UL, 0xA0A145UL, 0x26EDBEUL, 0x2A7448UL,
    0xAC38B3UL, 0x92C69DUL, 0x148A66UL, 0x181390UL, 0x9E5F6BUL, 0x01207CUL, 0x876C87UL, 0x8BF571UL, 0x0DB98AUL,
    0xF6092DUL, 0x7045D6UL, 0x7CDC20UL, 0xFA90DBUL, 0x65EFCCUL, 0xE3A337UL, 0xEF3AC1UL, 0x69763AUL, 0x578814UL,
    0xD1C4EFUL, 0xDD5D19UL, 0x5B11E2UL, 0xC46EF5UL, 0x42220EUL, 0x4EBBF8UL, 0xC8F703UL, 0x3F964DUL, 0xB9DAB6UL,
    0xB54340UL, 0x330FBBUL, 0xAC70ACUL, 0x2A3C57UL, 0x26A5A1UL, 0xA0E95AUL, 0x9E1774UL, 0x185B8FUL, 0x14C279UL,
    0x928E82UL, 0x0DF195UL, 0x8BBD6EUL, 0x872498UL, 0x016863UL, 0xFAD8C4UL, 0x7C943FUL, 0x700DC9UL, 0xF64132UL,
    0x693E25UL, 0xEF72DEUL, 0xE3EB28UL, 0x65A7D3UL, 0x5B59FDUL, 0xDD1506UL, 0xD18CF0UL, 0x57C00BUL, 0xC8BF1CUL,
    0x4EF3E7UL, 0x426A11UL, 0xC426EAUL, 0x2AE476UL, 0xACA88DUL, 0xA0317BUL, 0x267D80UL, 0xB90297UL, 0x3F4E6CUL,
    0x33D79AUL, 0xB59B61UL, 0x8B654FUL, 0x0D29B4UL, 0x01B042UL, 0x87FCB9UL, 0x1883AEUL, 0x9ECF55UL, 0x9256A3UL,
    0x141A58UL, 0xEFAAFFUL, 0x69E604UL, 0x657FF2UL, 0xE33309UL, 0x7C4C1EUL, 0xFA00E5UL, 0xF69913UL, 0x70D5E8UL,
    0x4E2BC6UL, 0xC8673DUL, 0xC4FECBUL, 0x42B230UL, 0xDDCD27UL, 0x5B81DCUL, 0x57182AUL, 0xD154D1UL, 0x26359FUL,
    0xA07964UL, 0xACE092UL, 0x2AAC69UL, 0xB5D37EUL, 0x339F85UL, 0x3F0673UL, 0xB94A88UL, 0x87B4A6UL, 0x01F85DUL,
    0x0D61ABUL, 0x8B2D50UL, 0x145247UL, 0x921EBCUL, 0x9E874AUL, 0x18CBB1UL, 0xE37B16UL, 0x6537EDUL, 0x69AE1BUL,
    0xEFE2E0UL, 0x709DF7UL, 0xF6D10CUL, 0xFA48FAUL, 0x7C0401UL, 0x42FA2FUL, 0xC4B6D4UL, 0xC82F22UL, 0x4E63D9UL,
    0xD11CCEUL, 0x575035UL, 0x5BC9C3UL, 0xDD8538UL,
};

/* NovAtel / Unicore 风格 CRC-32：反射 poly 0xEDB88320，init 0，不取反（与 zlib 的差异是
 * 不含初值取反与末尾取反）。表项 = 单字节反射移位 8 次的余数。 */
static const uint32_t s_crc32_table[256] = {
    0x00000000UL, 0x77073096UL, 0xEE0E612CUL, 0x990951BAUL, 0x076DC419UL, 0x706AF48FUL, 0xE963A535UL, 0x9E6495A3UL,
    0x0EDB8832UL, 0x79DCB8A4UL, 0xE0D5E91EUL, 0x97D2D988UL, 0x09B64C2BUL, 0x7EB17CBDUL, 0xE7B82D07UL, 0x90BF1D91UL,
    0x1DB71064UL, 0x6AB020F2UL, 0xF3B97148UL, 0x84BE41DEUL, 0x1ADAD47DUL, 0x6DDDE4EBUL, 0xF4D4B551UL, 0x83D385C7UL,
    0x136C9856UL, 0x646BA8C0UL, 0xFD62F97AUL, 0x8A65C9ECUL, 0x14015C4FUL, 0x63066CD9UL, 0xFA0F3D63UL, 0x8D080DF5UL,
    0x3B6E20C8UL, 0x4C69105EUL, 0xD56041E4UL, 0xA2677172UL, 0x3C03E4D1UL, 0x4B04D447UL, 0xD20D85FDUL, 0xA50AB56BUL,
    0x35B5A8FAUL, 0x42B2986CUL, 0xDBBBC9D6UL, 0xACBCF940UL, 0x32D86CE3UL, 0x45DF5C75UL, 0xDCD60DCFUL, 0xABD13D59UL,
    0x26D930ACUL, 0x51DE003AUL, 0xC8D75180UL, 0xBFD06116UL, 0x21B4F4B5UL, 0x56B3C423UL, 0xCFBA9599UL, 0xB8BDA50FUL,
    0x2802B89EUL, 0x5F058808UL, 0xC60CD9B2UL, 0xB10BE924UL, 0x2F6F7C87UL, 0x58684C11UL, 0xC1611DABUL, 0xB6662D3DUL,
    0x76DC4190UL, 0x01DB7106UL, 0x98D220BCUL, 0xEFD5102AUL, 0x71B18589UL, 0x06B6B51FUL, 0x9FBFE4A5UL, 0xE8B8D433UL,
    0x7807C9A2UL, 0x0F00F934UL, 0x9609A88EUL, 0xE10E9818UL, 0x7F6A0DBBUL, 0x086D3D2DUL, 0x91646C97UL, 0xE6635C01UL,
    0x6B6B51F4UL, 0x1C6C6162UL, 0x856530D8UL, 0xF262004EUL, 0x6C0695EDUL, 0x1B01A57BUL, 0x8208F4C1UL, 0xF50FC457UL,
    0x65B0D9C6UL, 0x12B7E950UL, 0x8BBEB8EAUL, 0xFCB9887CUL, 0x62DD1DDFUL, 0x15DA2D49UL, 0x8CD37CF3UL, 0xFBD44C65UL,
    0x4DB26158UL, 0x3AB551CEUL, 0xA3BC0074UL, 0xD4BB30E2UL, 0x4ADFA541UL, 0x3DD895D7UL, 0xA4D1C46DUL, 0xD3D6F4FBUL,
    0x4369E96AUL, 0x346ED9FCUL, 0xAD678846UL, 0xDA60B8D0UL, 0x44042D73UL, 0x33031DE5UL, 0xAA0A4C5FUL, 0xDD0D7CC9UL,
    0x5005713CUL, 0x270241AAUL, 0xBE0B1010UL, 0xC90C2086UL, 0x5768B525UL, 0x206F85B3UL, 0xB966D409UL, 0xCE61E49FUL,
    0x5EDEF90EUL, 0x29D9C998UL, 0xB0D09822UL, 0xC7D7A8B4UL, 0x59B33D17UL, 0x2EB40D81UL, 0xB7BD5C3BUL, 0xC0BA6CADUL,
    0xEDB88320UL, 0x9ABFB3B6UL, 0x03B6E20CUL, 0x74B1D29AUL, 0xEAD54739UL, 0x9DD277AFUL, 0x04DB2615UL, 0x73DC1683UL,
    0xE3630B12UL, 0x94643B84UL, 0x0D6D6A3EUL, 0x7A6A5AA8UL, 0xE40ECF0BUL, 0x9309FF9DUL, 0x0A00AE27UL, 0x7D079EB1UL,
    0xF00F9344UL, 0x8708A3D2UL, 0x1E01F268UL, 0x6906C2FEUL, 0xF762575DUL, 0x806567CBUL, 0x196C3671UL, 0x6E6B06E7UL,
    0xFED41B76UL, 0x89D32BE0UL, 0x10DA7A5AUL, 0x67DD4ACCUL, 0xF9B9DF6FUL, 0x8EBEEFF9UL, 0x17B7BE43UL, 0x60B08ED5UL,
    0xD6D6A3E8UL, 0xA1D1937EUL, 0x38D8C2C4UL, 0x4FDFF252UL, 0xD1BB67F1UL, 0xA6BC5767UL, 0x3FB506DDUL, 0x48B2364BUL,
    0xD80D2BDAUL, 0xAF0A1B4CUL, 0x36034AF6UL, 0x41047A60UL, 0xDF60EFC3UL, 0xA867DF55UL, 0x316E8EEFUL, 0x4669BE79UL,
    0xCB61B38CUL, 0xBC66831AUL, 0x256FD2A0UL, 0x5268E236UL, 0xCC0C7795UL, 0xBB0B4703UL, 0x220216B9UL, 0x5505262FUL,
    0xC5BA3BBEUL, 0xB2BD0B28UL, 0x2BB45A92UL, 0x5CB36A04UL, 0xC2D7FFA7UL, 0xB5D0CF31UL, 0x2CD99E8BUL, 0x5BDEAE1DUL,
    0x9B64C2B0UL, 0xEC63F226UL, 0x756AA39CUL, 0x026D930AUL, 0x9C0906A9UL, 0xEB0E363FUL, 0x72076785UL, 0x05005713UL,
    0x95BF4A82UL, 0xE2B87A14UL, 0x7BB12BAEUL, 0x0CB61B38UL, 0x92D28E9BUL, 0xE5D5BE0DUL, 0x7CDCEFB7UL, 0x0BDBDF21UL,
    0x86D3D2D4UL, 0xF1D4E242UL, 0x68DDB3F8UL, 0x1FDA836EUL, 0x81BE16CDUL, 0xF6B9265BUL, 0x6FB077E1UL, 0x18B74777UL,
    0x88085AE6UL, 0xFF0F6A70UL, 0x66063BCAUL, 0x11010B5CUL, 0x8F659EFFUL, 0xF862AE69UL, 0x616BFFD3UL, 0x166CCF45UL,
    0xA00AE278UL, 0xD70DD2EEUL, 0x4E048354UL, 0x3903B3C2UL, 0xA7672661UL, 0xD06016F7UL, 0x4969474DUL, 0x3E6E77DBUL,
    0xAED16A4AUL, 0xD9D65ADCUL, 0x40DF0B66UL, 0x37D83BF0UL, 0xA9BCAE53UL, 0xDEBB9EC5UL, 0x47B2CF7FUL, 0x30B5FFE9UL,
    0xBDBDF21CUL, 0xCABAC28AUL, 0x53B39330UL, 0x24B4A3A6UL, 0xBAD03605UL, 0xCDD70693UL, 0x54DE5729UL, 0x23D967BFUL,
    0xB3667A2EUL, 0xC4614AB8UL, 0x5D681B02UL, 0x2A6F2B94UL, 0xB40BBE37UL, 0xC30C8EA1UL, 0x5A05DF1BUL, 0x2D02EF8DUL,
};

/* ---------------------------------------------------------------- 基础工具 */

static bool is_printable(uint8_t byte)
{
    /* uint8_t 提升到 uint32_t 无损；显式无符号比较避免 int/unsigned 混用的有符号性告警 */
    uint32_t value = (uint32_t)byte;

    return (value >= RTK_DEMUX_PRIV_PRINTABLE_MIN) && (value <= RTK_DEMUX_PRIV_PRINTABLE_MAX);
}

static bool is_hex_digit(uint8_t byte)
{
    return ((byte >= (uint8_t)'0') && (byte <= (uint8_t)'9')) || ((byte >= (uint8_t)'A') && (byte <= (uint8_t)'F')) ||
           ((byte >= (uint8_t)'a') && (byte <= (uint8_t)'f'));
}

static bool is_upper_alpha(uint8_t byte)
{
    return (byte >= (uint8_t)'A') && (byte <= (uint8_t)'Z');
}

static bool is_upper_alnum(uint8_t byte)
{
    return is_upper_alpha(byte) || ((byte >= (uint8_t)'0') && (byte <= (uint8_t)'9'));
}

/* 调用方必须先用 is_hex_digit() 判定；返回值 0..15。 */
static uint8_t hex_value(uint8_t byte)
{
    uint8_t value = 0U;

    if ((byte >= (uint8_t)'0') && (byte <= (uint8_t)'9')) {
        /* 差 0..9，窄化到 uint8_t 安全 */
        value = (uint8_t)(byte - (uint8_t)'0');
    } else if ((byte >= (uint8_t)'A') && (byte <= (uint8_t)'F')) {
        /* 差 0..5，加 RTK_DEMUX_PRIV_HEX_ALPHA_BASE 后 ≤ 15，窄化安全 */
        value = (uint8_t)((uint32_t)(byte - (uint8_t)'A') + RTK_DEMUX_PRIV_HEX_ALPHA_BASE);
    } else {
        /* 小写形式；同上，≤ 15 */
        value = (uint8_t)((uint32_t)(byte - (uint8_t)'a') + RTK_DEMUX_PRIV_HEX_ALPHA_BASE);
    }
    return value;
}

static uint32_t read_u16_le(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < RTK_DEMUX_PRIV_U16_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (RTK_DEMUX_PRIV_BYTE_BITS * idx);
    }
    return value;
}

static uint32_t read_u32_le(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < RTK_DEMUX_PRIV_U32_BYTES; idx++) {
        /* uint8_t 提升到 uint32_t 无损 */
        value |= (uint32_t)buf[idx] << (RTK_DEMUX_PRIV_BYTE_BITS * idx);
    }
    return value;
}

/* 读大端 3 B（RTCM3 的 CRC 字段）。 */
static uint32_t read_u24_be(const uint8_t *buf)
{
    uint32_t value = 0U;

    for (uint32_t idx = 0U; idx < RTK_DEMUX_PRIV_U24_BYTES; idx++) {
        /* 先左移再或入，避免有符号中间值 */
        value = (value << RTK_DEMUX_PRIV_BYTE_BITS) | (uint32_t)buf[idx];
    }
    return value;
}

static uint8_t xor8(const uint8_t *buf, size_t len)
{
    uint8_t acc = 0U;

    for (size_t idx = 0U; idx < len; idx++) {
        acc = (uint8_t)(acc ^ buf[idx]);
    }
    return acc;
}

/* UBX 8 位 Fletcher：CK_A 累加字节，CK_B 累加 CK_A，均在模 256 下（uint8_t 自然截断）。 */
static void fletcher8(const uint8_t *buf, size_t from, size_t len, uint8_t *ck_a, uint8_t *ck_b)
{
    uint8_t a = 0U;
    uint8_t b = 0U;

    for (size_t idx = 0U; idx < len; idx++) {
        /* 模 256 截断是本算法定义，窄化安全 */
        a = (uint8_t)(a + buf[from + idx]);
        b = (uint8_t)(b + a);
    }
    *ck_a = a;
    *ck_b = b;
}

static uint32_t crc24q(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0U;

    for (size_t idx = 0U; idx < len; idx++) {
        /* 取 crc 的高 8 位与数据字节异或作表下标（0..255）；左移 8 位后取低 24 位 */
        uint32_t index = ((crc >> (RTK_DEMUX_PRIV_BYTE_BITS * RTK_DEMUX_PRIV_U16_BYTES)) ^ (uint32_t)buf[idx]) &
                         RTK_DEMUX_PRIV_BYTE_MASK;

        crc = ((crc << RTK_DEMUX_PRIV_BYTE_BITS) & RTK_DEMUX_PRIV_CRC24Q_MASK) ^ s_crc24q_table[index];
    }
    return crc;
}

static uint32_t crc32_novatel(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0U;

    for (size_t idx = 0U; idx < len; idx++) {
        uint32_t index = (crc ^ (uint32_t)buf[idx]) & RTK_DEMUX_PRIV_BYTE_MASK;

        crc = (crc >> RTK_DEMUX_PRIV_BYTE_BITS) ^ s_crc32_table[index];
    }
    return crc;
}

/* 在 frame[last-back .. last] 内反向寻找最近的 '*'（back 为相对 last 的偏移，0 = last 自身）。
 * 窗口长度固定，因此是 O(1)；用于在不知道帧长时只检查最近几个字节。 */
static bool find_star_reverse(const uint8_t *frame, size_t last, size_t window, size_t *ofs)
{
    size_t back = 0U;
    bool found = false;

    *ofs = 0U;
    while ((back <= window) && (back <= last)) {
        if (frame[last - back] == RTK_DEMUX_PRIV_STAR) {
            *ofs = back;
            found = true;
            break;
        }
        back++;
    }
    return found;
}

/* ASCII 正文合法性：可打印且不等于 '$'（NMEA/Unicore 共有的约束）。 */
static bool is_ascii_body(uint8_t byte)
{
    return is_printable(byte) && (byte != RTK_DEMUX_PRIV_SYNC_NMEA);
}

/* ---------------------------------------------------------------- RTCM3 */

static bool rtcm3_first_byte(uint8_t byte)
{
    return byte == RTK_DEMUX_PRIV_SYNC_RTCM3;
}

static rtk_demux_step_t rtcm3_step(const uint8_t *frame, size_t len, size_t *need)
{
    uint32_t payload_len = 0U;
    size_t total = 0U;
    uint32_t crc_calc = 0U;
    uint32_t crc_rx = 0U;

    /* len ≥ 1 由分流器保证（首字节已通过 first_byte）。 */
    if (len < RTK_DEMUX_PRIV_U16_BYTES) {
        *need = (size_t)RTK_DEMUX_PRIV_U16_BYTES - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if ((frame[1] & RTK_DEMUX_PRIV_RTCM3_RESERVED_MASK) != 0U) {
        /* 高 6 位保留位必须为 0：RTCM 与任何其他 0xD3 数据在这里区分 */
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < RTK_DEMUX_PRIV_RTCM3_HDR_LEN) {
        *need = (size_t)RTK_DEMUX_PRIV_RTCM3_HDR_LEN - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }

    /* 长度 10 bit：最大 1023，总长 = 3 + 1023 + 3 = 1029 = max_frame，无需额外超限判断 */
    payload_len =
        (((uint32_t)frame[1] & RTK_DEMUX_PRIV_RTCM3_LEN_HI_MASK) << RTK_DEMUX_PRIV_BYTE_BITS) | (uint32_t)frame[2];
    total = (size_t)payload_len + (size_t)RTK_DEMUX_PRIV_RTCM3_HDR_LEN + (size_t)RTK_DEMUX_PRIV_RTCM3_CRC_LEN;

    if (len < total) {
        *need = total - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (len > total) {
        /* 不可能由分流器产生（FRAME 后即复位）；防御性拒绝而非越界读 */
        return RTK_DEMUX_STEP_REJECT;
    }

    crc_calc = crc24q(frame, total - (size_t)RTK_DEMUX_PRIV_RTCM3_CRC_LEN);
    crc_rx = read_u24_be(&frame[total - (size_t)RTK_DEMUX_PRIV_RTCM3_CRC_LEN]);

    return (crc_calc == crc_rx) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

/* ---------------------------------------------------------------- NMEA 0183 */

static bool nmea_first_byte(uint8_t byte)
{
    return byte == RTK_DEMUX_PRIV_SYNC_NMEA;
}

/* NMEA 结束符到达：整段复核尾部结构、正文合法性与 XOR 校验（覆盖 '$' 与 '*' 之间）。
 * 只用 frame[0..len)，因此 FRAME 结论与调用历史无关。 */
static rtk_demux_step_t nmea_finish(const uint8_t *frame, size_t len)
{
    size_t last = len - 1U;
    size_t star = last - (size_t)RTK_DEMUX_PRIV_NMEA_OFS_STAR;
    uint8_t checksum_calc = 0U;
    uint8_t checksum_rx = 0U;

    if ((len < (size_t)RTK_DEMUX_PRIV_NMEA_MIN_LEN) || (frame[last - 1U] != RTK_DEMUX_PRIV_CR) ||
        (!is_hex_digit(frame[last - 2U])) || (!is_hex_digit(frame[last - RTK_DEMUX_PRIV_NMEA_OFS_HEX_HI])) ||
        (frame[star] != RTK_DEMUX_PRIV_STAR)) {
        return RTK_DEMUX_STEP_REJECT;
    }
    for (size_t idx = 1U; idx < star; idx++) {
        /* 正文不得出现 '$' '*'（'*' 已由尾部结构消费） */
        if ((!is_ascii_body(frame[idx])) || (frame[idx] == RTK_DEMUX_PRIV_STAR)) {
            return RTK_DEMUX_STEP_REJECT;
        }
    }
    checksum_calc = xor8(&frame[1], star - 1U);
    /* 两位十六进制拼成一个字节：各 ≤ 0x0F，高位移 4 位后 ≤ 0xF0，结果 ≤ 0xFF，窄化安全 */
    checksum_rx = (uint8_t)(((uint32_t)hex_value(frame[star + 1U]) << RTK_DEMUX_PRIV_NIBBLE_BITS) |
                            (uint32_t)hex_value(frame[star + 2U]));
    return (checksum_calc == checksum_rx) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

static rtk_demux_step_t nmea_step(const uint8_t *frame, size_t len, size_t *need)
{
    size_t last = len - 1U; /* len ≥ 1 */
    uint8_t new_byte = frame[last];
    size_t star_ofs = 0U;

    if (len < RTK_DEMUX_PRIV_U16_BYTES) {
        *need = (size_t)RTK_DEMUX_PRIV_U16_BYTES - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (!is_upper_alnum(frame[1])) {
        /* 第 2 字节必须是发送方标识（大写字母或数字），用于与同以 '$' 开头的其他协议区分 */
        return RTK_DEMUX_STEP_REJECT;
    }

    if (new_byte == RTK_DEMUX_PRIV_LF) {
        return nmea_finish(frame, len);
    }

    /* 未到结束符：只检查新到字节及其前面定长窗口内的 '*' 位置（O(1)） */
    if (find_star_reverse(frame, last, RTK_DEMUX_PRIV_NMEA_STAR_WINDOW, &star_ofs)) {
        if (star_ofs == 0U) {
            /* 当前字节就是 '*'，校验字段从此开始 */
        } else if (star_ofs < (size_t)RTK_DEMUX_PRIV_NMEA_OFS_HEX_HI) {
            /* 偏移 1、2：两位十六进制校验位 */
            if (!is_hex_digit(new_byte)) {
                return RTK_DEMUX_STEP_REJECT;
            }
        } else if (star_ofs == (size_t)RTK_DEMUX_PRIV_NMEA_OFS_HEX_HI) {
            /* 偏移 3：只能是 '\r' */
            if (new_byte != RTK_DEMUX_PRIV_CR) {
                return RTK_DEMUX_STEP_REJECT;
            }
        } else {
            /* 偏移 4 只可能是 '\n'（上面已处理），此处即非法 */
            return RTK_DEMUX_STEP_REJECT;
        }
    } else if (!is_ascii_body(new_byte)) {
        return RTK_DEMUX_STEP_REJECT;
    } else {
        /* 正文字节合法，继续累积 */
    }

    if ((len + 1U) > (size_t)RTK_DEMUX_PRIV_NMEA_MAX_FRAME) {
        /* 超过 max_frame 仍未结束：need 契约禁止要求超限字节，只能拒绝 */
        return RTK_DEMUX_STEP_REJECT;
    }
    *need = 1U;
    return RTK_DEMUX_STEP_NEED_MORE;
}

/* ---------------------------------------------------------------- u-blox UBX */

static bool ubx_first_byte(uint8_t byte)
{
    return byte == RTK_DEMUX_PRIV_SYNC_UBX_A;
}

static rtk_demux_step_t ubx_step(const uint8_t *frame, size_t len, size_t *need)
{
    uint32_t payload_len = 0U;
    size_t total = 0U;
    uint8_t ck_a = 0U;
    uint8_t ck_b = 0U;

    if (len < RTK_DEMUX_PRIV_U16_BYTES) {
        *need = (size_t)RTK_DEMUX_PRIV_U16_BYTES - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (frame[1] != RTK_DEMUX_PRIV_SYNC_UBX_B) {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < (size_t)RTK_DEMUX_PRIV_UBX_HDR_LEN) {
        *need = (size_t)RTK_DEMUX_PRIV_UBX_HDR_LEN - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }

    payload_len = read_u16_le(&frame[RTK_DEMUX_PRIV_UBX_OFS_LEN]);
    /* payload_len ≤ 0xFFFF，加固定部分后仍在 size_t 范围内 */
    total = (size_t)RTK_DEMUX_PRIV_UBX_HDR_LEN + (size_t)payload_len + (size_t)RTK_DEMUX_PRIV_UBX_CK_LEN;
    if (total > (size_t)RTK_DEMUX_PRIV_UBX_MAX_FRAME) {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < total) {
        *need = total - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (len > total) {
        return RTK_DEMUX_STEP_REJECT;
    }

    /* CK 覆盖 class..载荷，即 [2, total-2) */
    fletcher8(frame, (size_t)RTK_DEMUX_PRIV_UBX_SYNC_LEN,
              total - (size_t)RTK_DEMUX_PRIV_UBX_SYNC_LEN - (size_t)RTK_DEMUX_PRIV_UBX_CK_LEN, &ck_a, &ck_b);

    if ((ck_a != frame[total - 2U]) || (ck_b != frame[total - 1U])) {
        return RTK_DEMUX_STEP_REJECT;
    }
    return RTK_DEMUX_STEP_FRAME;
}

/* ---------------------------------------------------------------- Unicore 二进制 */

static bool unicore_bin_first_byte(uint8_t byte)
{
    return byte == RTK_DEMUX_PRIV_SYNC_UNICORE_A;
}

static rtk_demux_step_t unicore_bin_step(const uint8_t *frame, size_t len, size_t *need)
{
    uint32_t payload_len = 0U;
    size_t total = 0U;
    uint32_t crc_calc = 0U;
    uint32_t crc_rx = 0U;

    if (len < RTK_DEMUX_PRIV_U16_BYTES) {
        *need = (size_t)RTK_DEMUX_PRIV_U16_BYTES - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (frame[1] != RTK_DEMUX_PRIV_SYNC_UNICORE_B) {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < (size_t)RTK_DEMUX_PRIV_U24_BYTES) {
        *need = (size_t)RTK_DEMUX_PRIV_U24_BYTES - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (frame[2] != RTK_DEMUX_PRIV_SYNC_UNICORE_C) {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < ((size_t)RTK_DEMUX_PRIV_UCBIN_OFS_LEN + (size_t)RTK_DEMUX_PRIV_U16_BYTES)) {
        *need = ((size_t)RTK_DEMUX_PRIV_UCBIN_OFS_LEN + (size_t)RTK_DEMUX_PRIV_U16_BYTES) - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }

    payload_len = read_u16_le(&frame[RTK_DEMUX_PRIV_UCBIN_OFS_LEN]);
    /* payload_len ≤ 0xFFFF，加固定头与 CRC 后仍在 size_t 范围内 */
    total = (size_t)RTK_DEMUX_PRIV_UCBIN_HDR_LEN + (size_t)payload_len + (size_t)RTK_DEMUX_PRIV_UCBIN_CRC_LEN;
    if (total > (size_t)RTK_DEMUX_PRIV_UNICORE_BIN_MAX_FRAME) {
        return RTK_DEMUX_STEP_REJECT;
    }
    if (len < total) {
        *need = total - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (len > total) {
        return RTK_DEMUX_STEP_REJECT;
    }

    /* CRC-32 覆盖头部 + 载荷 */
    crc_calc = crc32_novatel(frame, total - (size_t)RTK_DEMUX_PRIV_UCBIN_CRC_LEN);
    crc_rx = read_u32_le(&frame[total - (size_t)RTK_DEMUX_PRIV_UCBIN_CRC_LEN]);

    return (crc_calc == crc_rx) ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

/* ---------------------------------------------------------------- Unicore ASCII(Hash) */

static bool unicore_hash_first_byte(uint8_t byte)
{
    return byte == RTK_DEMUX_PRIV_SYNC_HASH;
}

/* Hash 正文更严格：不含 '#' '$' '*'。 */
static bool is_hash_body(uint8_t byte)
{
    return is_ascii_body(byte) && (byte != RTK_DEMUX_PRIV_SYNC_HASH) && (byte != RTK_DEMUX_PRIV_STAR);
}

/* 读 count 位大端十六进制（count ≤ 8）；调用方保证范围合法。 */
static uint32_t read_hex_be(const uint8_t *frame, size_t from, size_t count)
{
    uint32_t value = 0U;

    for (size_t idx = 0U; idx < count; idx++) {
        value = (value << 4U) | (uint32_t)hex_value(frame[from + idx]);
    }
    return value;
}

static bool all_hex(const uint8_t *frame, size_t from, size_t count)
{
    bool ok = true;

    for (size_t idx = 0U; idx < count; idx++) {
        if (!is_hex_digit(frame[from + idx])) {
            ok = false;
            break;
        }
    }
    return ok;
}

/* Hash 结束符到达时的两种校验形式：'*' + 2 位十六进制 XOR，或 '*' + 8 位十六进制 CRC-32。
 * 正文与校验值都用 frame[0..len) 复核，保证 FRAME 结论只取决于入参。 */
static rtk_demux_step_t hash_finish(const uint8_t *frame, size_t len, size_t star)
{
    bool checksum_ok = false;

    for (size_t idx = 1U; idx < star; idx++) {
        if (!is_hash_body(frame[idx])) {
            return RTK_DEMUX_STEP_REJECT;
        }
    }

    if ((len - 1U - star) == (size_t)RTK_DEMUX_PRIV_HASH_OFS_STAR_XOR) {
        /* 形式一：XOR，覆盖 '#' 与 '*' 之间 */
        checksum_ok = (xor8(&frame[1], star - 1U) ==
                       (uint8_t)(((uint32_t)hex_value(frame[star + 1U]) << RTK_DEMUX_PRIV_NIBBLE_BITS) |
                                 (uint32_t)hex_value(frame[star + 2U])));
    } else {
        /* 形式二：CRC-32，同 Unicore 二进制的算法 */
        checksum_ok = (crc32_novatel(&frame[1], star - 1U) ==
                       read_hex_be(frame, star + 1U, (size_t)RTK_DEMUX_PRIV_HASH_CRC_DIGITS));
    }
    return checksum_ok ? RTK_DEMUX_STEP_FRAME : RTK_DEMUX_STEP_REJECT;
}

/* 未到结束符时，只校验新到字节相对 '*' 的位置（O(1)）。star 为 '*' 的下标，new_byte 为 frame[last]。 */
static bool hash_mid_byte_ok(const uint8_t *frame, size_t last, size_t star, uint8_t new_byte)
{
    size_t after_star = last - star; /* ≥ 1：star_ofs == 0（当前字节是 '*'）由调用方单独放行 */

    if (after_star <= (size_t)RTK_DEMUX_PRIV_XOR_DIGITS) {
        /* 两种形式的前两位都是十六进制 */
        return is_hex_digit(new_byte);
    }
    if (after_star == (size_t)RTK_DEMUX_PRIV_HASH_OFS_BRANCH) {
        /* XOR 形式到此结束为 '\r'；CRC 形式继续是十六进制 */
        return is_hex_digit(new_byte) || (new_byte == RTK_DEMUX_PRIV_CR);
    }
    if (after_star <= (size_t)RTK_DEMUX_PRIV_HASH_CRC_DIGITS) {
        /* 4..8：只有 CRC 形式能走到这里；'*' 后第 3 字节已是 '\r' 说明 XOR 形式早该结束 */
        if (frame[star + (size_t)RTK_DEMUX_PRIV_HASH_OFS_BRANCH] == RTK_DEMUX_PRIV_CR) {
            return false;
        }
        return is_hex_digit(new_byte);
    }
    if (after_star == (size_t)RTK_DEMUX_PRIV_HASH_OFS_CR) {
        /* CRC 形式 8 位十六进制之后的 '\r' */
        if (frame[star + (size_t)RTK_DEMUX_PRIV_HASH_OFS_BRANCH] == RTK_DEMUX_PRIV_CR) {
            return false;
        }
        return new_byte == RTK_DEMUX_PRIV_CR;
    }
    /* 更远的偏移只可能是 '\n'（结束符由调用方单独处理） */
    return false;
}

static rtk_demux_step_t unicore_hash_step(const uint8_t *frame, size_t len, size_t *need)
{
    size_t last = len - 1U; /* len ≥ 1 */
    uint8_t new_byte = frame[last];
    size_t star_ofs = 0U;
    size_t star = 0U;

    if (len < RTK_DEMUX_PRIV_U16_BYTES) {
        *need = (size_t)RTK_DEMUX_PRIV_U16_BYTES - len;
        return RTK_DEMUX_STEP_NEED_MORE;
    }
    if (!is_upper_alpha(frame[1])) {
        /* 第 2 字节必须是大写字母（语句名首字母），用于与同以 '#' 开头的其他语法区分 */
        return RTK_DEMUX_STEP_REJECT;
    }

    if (new_byte == RTK_DEMUX_PRIV_LF) {
        /* 最近的 '*' 就是校验字段的起点；两种形式的 '*' 偏移都是定值 */
        if (!find_star_reverse(frame, last, RTK_DEMUX_PRIV_HASH_STAR_WINDOW, &star_ofs)) {
            return RTK_DEMUX_STEP_REJECT;
        }
        if ((star_ofs != (size_t)RTK_DEMUX_PRIV_HASH_OFS_STAR_XOR) &&
            (star_ofs != (size_t)RTK_DEMUX_PRIV_HASH_OFS_STAR_CRC)) {
            return RTK_DEMUX_STEP_REJECT;
        }
        if (frame[last - 1U] != RTK_DEMUX_PRIV_CR) {
            return RTK_DEMUX_STEP_REJECT;
        }
        star = last - star_ofs;
        if (star_ofs == (size_t)RTK_DEMUX_PRIV_HASH_OFS_STAR_XOR) {
            if ((!is_hex_digit(frame[star + 1U])) || (!is_hex_digit(frame[star + 2U]))) {
                return RTK_DEMUX_STEP_REJECT;
            }
        } else if (!all_hex(frame, star + 1U, (size_t)RTK_DEMUX_PRIV_HASH_CRC_DIGITS)) {
            return RTK_DEMUX_STEP_REJECT;
        }
        return hash_finish(frame, len, star);
    }

    /* 未到结束符：只检查新到字节及其前面定长窗口内的 '*' 位置（O(1)） */
    if (!find_star_reverse(frame, last, RTK_DEMUX_PRIV_HASH_STAR_WINDOW, &star_ofs)) {
        if (!is_hash_body(new_byte)) {
            return RTK_DEMUX_STEP_REJECT;
        }
    } else if ((star_ofs != 0U) && (!hash_mid_byte_ok(frame, last, last - star_ofs, new_byte))) {
        /* 偏移 0 是 '*' 本身（校验字段开始）；其余偏移由 hash_mid_byte_ok 判定 */
        return RTK_DEMUX_STEP_REJECT;
    } else {
        /* 正文字节或 '*' 合法，继续累积 */
    }

    if ((len + 1U) > (size_t)RTK_DEMUX_PRIV_UNICORE_HASH_MAX_FRAME) {
        return RTK_DEMUX_STEP_REJECT;
    }
    *need = 1U;
    return RTK_DEMUX_STEP_NEED_MORE;
}

/* ---------------------------------------------------------------- 描述符 */

const rtk_demux_decoder_t rtk_demux_dec_rtcm3 = {
    .name = "rtcm3",
    .proto_id = RTK_DEMUX_PROTO_RTCM3,
    .max_frame = RTK_DEMUX_PRIV_RTCM3_MAX_FRAME,
    .first_byte = rtcm3_first_byte,
    .step = rtcm3_step,
};

const rtk_demux_decoder_t rtk_demux_dec_nmea = {
    .name = "nmea",
    .proto_id = RTK_DEMUX_PROTO_NMEA,
    .max_frame = RTK_DEMUX_PRIV_NMEA_MAX_FRAME,
    .first_byte = nmea_first_byte,
    .step = nmea_step,
};

const rtk_demux_decoder_t rtk_demux_dec_ubx = {
    .name = "ubx",
    .proto_id = RTK_DEMUX_PROTO_UBX,
    .max_frame = RTK_DEMUX_PRIV_UBX_MAX_FRAME,
    .first_byte = ubx_first_byte,
    .step = ubx_step,
};

const rtk_demux_decoder_t rtk_demux_dec_unicore_bin = {
    .name = "unicore_bin",
    .proto_id = RTK_DEMUX_PROTO_UNICORE_BIN,
    .max_frame = RTK_DEMUX_PRIV_UNICORE_BIN_MAX_FRAME,
    .first_byte = unicore_bin_first_byte,
    .step = unicore_bin_step,
};

const rtk_demux_decoder_t rtk_demux_dec_unicore_hash = {
    .name = "unicore_hash",
    .proto_id = RTK_DEMUX_PROTO_UNICORE_HASH,
    .max_frame = RTK_DEMUX_PRIV_UNICORE_HASH_MAX_FRAME,
    .first_byte = unicore_hash_first_byte,
    .step = unicore_hash_step,
};
