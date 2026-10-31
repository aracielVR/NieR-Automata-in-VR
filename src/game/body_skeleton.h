// Armature raw data
#pragma once
#include <iterator>
#include <stdint.h>
#include "core/vecmath.h"

namespace pure {

// clang-format off (one line per bone, one per part)
// Her bones: the game's bone id, the parent (an index here, -1: none), the bind pose's position (metres).
struct SkeletonBone {
	int16_t id;
	int16_t parent;
	V3 bind;
};
inline constexpr SkeletonBone kSkeleton[] = {
	{4095, -1, {0, 1.0161f, 0}},  // 0: A root at hip height: may be her centre for the game's movement
	{4094, -1, {0, 0, 0}},  // 1: The root on the floor: the model's origin
	{0, 1, {0, 1.0160f, 0}},  // 2: Hips
	{1, 2, {0, 1.0160f, 0}},  // 3: Waist
	{2, 3, {0, 1.1430f, 0.0100f}},  // 4: Spine
	{3, 4, {0, 1.2810f, -0.0050f}},  // 5: Chest
	{4, 5, {0, 1.4150f, -0.0490f}},  // 6: Neck
	{5, 6, {0, 1.4900f, -0.0190f}},  // 7: Head
	{2816, 7, {0, 1.5300f, 0.0100f}},  // 8: Centre of her face: may be the jaw or the face's root
	{2818, 7, {-0.0290f, 1.5676f, 0.0631f}},  // 9: Right eye
	{2817, 7, {0.0290f, 1.5676f, 0.0631f}},  // 10: Left eye
	{3152, 7, {0, 1.5140f, -0.0210f}},  // 11: Back of the head, low: may be a hair root
	{3156, 7, {0.0478f, 1.6385f, 0.0780f}},  // 12: Hair: left fringe strand, joint 1
	{3157, 12, {0.0612f, 1.5739f, 0.0780f}},  // 13: Hair: left fringe strand, joint 2
	{3158, 13, {0.0609f, 1.5303f, 0.0730f}},  // 14: Hair: left fringe strand, joint 3
	{3159, 14, {0.0542f, 1.5003f, 0.0680f}},  // 15: Hair: left fringe strand, joint 4
	{3160, 7, {-0.0478f, 1.6385f, 0.0780f}},  // 16: Hair: right fringe strand, joint 1
	{3161, 16, {-0.0612f, 1.5739f, 0.0730f}},  // 17: Hair: right fringe strand, joint 2
	{3162, 17, {-0.0609f, 1.5303f, 0.0680f}},  // 18: Hair: right fringe strand, joint 3
	{3163, 18, {-0.0523f, 1.4920f, 0.0630f}},  // 19: Hair: right fringe strand, joint 4
	{3153, 7, {-0.0199f, 1.6273f, 0.1028f}},  // 20: Hair: fringe strand in front, joint 1
	{3154, 20, {-0.0199f, 1.5881f, 0.1028f}},  // 21: Hair: fringe strand in front, joint 2
	{3155, 21, {-0.0199f, 1.5553f, 0.1028f}},  // 22: Hair: fringe strand in front, joint 3
	{3164, 7, {0.0850f, 1.6385f, -0.0015f}},  // 23: Hair: left side strand, joint 1
	{3165, 23, {0.0850f, 1.5585f, -0.0015f}},  // 24: Hair: left side strand, joint 2
	{3166, 7, {0.0650f, 1.6385f, -0.0740f}},  // 25: Hair: back left strand, joint 1
	{3167, 25, {0.0650f, 1.5585f, -0.0740f}},  // 26: Hair: back left strand, joint 2
	{3170, 7, {-0.0650f, 1.6385f, -0.0740f}},  // 27: Hair: back right strand, joint 1
	{3171, 27, {-0.0650f, 1.5585f, -0.0740f}},  // 28: Hair: back right strand, joint 2
	{3172, 7, {-0.0850f, 1.6385f, -0.0015f}},  // 29: Hair: right side strand, joint 1
	{3173, 29, {-0.0850f, 1.5585f, -0.0015f}},  // 30: Hair: right side strand, joint 2
	{3168, 7, {0, 1.6385f, -0.0890f}},  // 31: Hair: back strand, joint 1
	{3169, 31, {0, 1.5585f, -0.0890f}},  // 32: Hair: back strand, joint 2
	{2640, 6, {0, 1.4650f, -0.0255f}},  // 33: Neck helper: may keep her neck smooth when her head turns
	{6, 5, {-0.0330f, 1.3710f, -0.0290f}},  // 34: Right clavicle
	{7, 34, {-0.1294f, 1.3434f, -0.0430f}},  // 35: Right upper arm
	{8, 35, {-0.2953f, 1.2041f, -0.0048f}},  // 36: Right forearm
	{9, 36, {-0.4413f, 1.0816f, 0.1052f}},  // 37: Right hand
	{256, 37, {-0.4312f, 1.0725f, 0.1342f}},  // 38: Right thumb, joint 1
	{257, 38, {-0.4310f, 1.0554f, 0.1631f}},  // 39: Right thumb, joint 2
	{258, 39, {-0.4371f, 1.0389f, 0.1822f}},  // 40: Right thumb, joint 3
	{259, 37, {-0.4507f, 1.0662f, 0.1405f}},  // 41: Right index finger, joint 1
	{260, 41, {-0.4703f, 1.0463f, 0.1636f}},  // 42: Right index finger, joint 2
	{261, 42, {-0.4794f, 1.0177f, 0.1841f}},  // 43: Right index finger, joint 3
	{262, 43, {-0.4838f, 1.0010f, 0.1941f}},  // 44: Right index finger, joint 4
	{263, 37, {-0.4565f, 1.0614f, 0.1275f}},  // 45: Right middle finger, joint 1
	{264, 45, {-0.4774f, 1.0373f, 0.1467f}},  // 46: Right middle finger, joint 2
	{265, 46, {-0.4927f, 1.0076f, 0.1644f}},  // 47: Right middle finger, joint 3
	{266, 47, {-0.4949f, 0.9862f, 0.1734f}},  // 48: Right middle finger, joint 4
	{267, 37, {-0.4626f, 1.0563f, 0.1137f}},  // 49: Right ring finger, joint 1
	{268, 49, {-0.4819f, 1.0323f, 0.1293f}},  // 50: Right ring finger, joint 2
	{269, 50, {-0.4976f, 1.0023f, 0.1456f}},  // 51: Right ring finger, joint 3
	{270, 51, {-0.4996f, 0.9818f, 0.1541f}},  // 52: Right ring finger, joint 4
	{271, 37, {-0.4672f, 1.0524f, 0.1033f}},  // 53: Right little finger, joint 1
	{272, 53, {-0.4839f, 1.0269f, 0.1091f}},  // 54: Right little finger, joint 2
	{273, 54, {-0.4969f, 1.0026f, 0.1209f}},  // 55: Right little finger, joint 3
	{274, 55, {-0.5020f, 0.9866f, 0.1274f}},  // 56: Right little finger, joint 4
	{2577, 36, {-0.4419f, 1.0811f, 0.1057f}},  // 57: Right wrist helper: may be wrist twist
	{2576, 36, {-0.3617f, 1.1484f, 0.0452f}},  // 58: Right forearm helper: may be forearm twist
	{3121, 36, {-0.3665f, 1.1475f, 0.0403f}},  // 59: On the right forearm: may be where a weapon or bracelet attaches
	{2560, 34, {-0.1294f, 1.3435f, -0.0430f}},  // 60: Right shoulder helper: may be upper arm twist
	{10, 5, {0.0330f, 1.3710f, -0.0290f}},  // 61: Left clavicle
	{11, 61, {0.1294f, 1.3434f, -0.0430f}},  // 62: Left upper arm
	{12, 62, {0.2953f, 1.2041f, -0.0048f}},  // 63: Left forearm
	{13, 63, {0.4413f, 1.0816f, 0.1052f}},  // 64: Left hand
	{527, 64, {0.4672f, 1.0524f, 0.1033f}},  // 65: Left little finger, joint 1
	{528, 65, {0.4839f, 1.0269f, 0.1091f}},  // 66: Left little finger, joint 2
	{529, 66, {0.4969f, 1.0026f, 0.1209f}},  // 67: Left little finger, joint 3
	{530, 67, {0.5020f, 0.9866f, 0.1274f}},  // 68: Left little finger, joint 4
	{523, 64, {0.4626f, 1.0563f, 0.1137f}},  // 69: Left ring finger, joint 1
	{524, 69, {0.4819f, 1.0323f, 0.1293f}},  // 70: Left ring finger, joint 2
	{525, 70, {0.4976f, 1.0023f, 0.1456f}},  // 71: Left ring finger, joint 3
	{526, 71, {0.4996f, 0.9818f, 0.1541f}},  // 72: Left ring finger, joint 4
	{519, 64, {0.4565f, 1.0614f, 0.1275f}},  // 73: Left middle finger, joint 1
	{520, 73, {0.4774f, 1.0373f, 0.1467f}},  // 74: Left middle finger, joint 2
	{521, 74, {0.4927f, 1.0076f, 0.1644f}},  // 75: Left middle finger, joint 3
	{522, 75, {0.4949f, 0.9862f, 0.1734f}},  // 76: Left middle finger, joint 4
	{515, 64, {0.4507f, 1.0662f, 0.1405f}},  // 77: Left index finger, joint 1
	{516, 77, {0.4703f, 1.0463f, 0.1636f}},  // 78: Left index finger, joint 2
	{517, 78, {0.4794f, 1.0177f, 0.1841f}},  // 79: Left index finger, joint 3
	{518, 79, {0.4838f, 1.0010f, 0.1941f}},  // 80: Left index finger, joint 4
	{512, 64, {0.4312f, 1.0725f, 0.1342f}},  // 81: Left thumb, joint 1
	{513, 81, {0.4310f, 1.0554f, 0.1631f}},  // 82: Left thumb, joint 2
	{514, 82, {0.4371f, 1.0389f, 0.1822f}},  // 83: Left thumb, joint 3
	{2593, 63, {0.4419f, 1.0811f, 0.1057f}},  // 84: Left wrist helper: may be wrist twist
	{2592, 63, {0.3617f, 1.1484f, 0.0452f}},  // 85: Left forearm helper: may be forearm twist
	{3120, 63, {0.3665f, 1.1475f, 0.0403f}},  // 86: On the left forearm: may be where a weapon or bracelet attaches
	{2561, 61, {0.1294f, 1.3435f, -0.0430f}},  // 87: Left shoulder helper: may be upper arm twist
	{3136, 5, {0, 1.2810f, 0.1000f}},  // 88: Front of her chest: may be breast movement
	{14, 2, {0, 1.0160f, 0}},  // 89: Pelvis
	{15, 89, {-0.1010f, 0.9510f, 0}},  // 90: Right thigh
	{16, 90, {-0.1341f, 0.5724f, -0.0100f}},  // 91: Right shin
	{17, 91, {-0.1690f, 0.1740f, -0.0200f}},  // 92: Right ankle
	{18, 92, {-0.1690f, 0.0390f, 0.0600f}},  // 93: Right toes
	{19, 89, {0.1010f, 0.9510f, 0}},  // 94: Left thigh
	{20, 94, {0.1341f, 0.5724f, -0.0100f}},  // 95: Left shin
	{21, 95, {0.1690f, 0.1740f, -0.0200f}},  // 96: Left ankle
	{22, 96, {0.1690f, 0.0390f, 0.0600f}},  // 97: Left toes
	{2608, 89, {-0.0660f, 0.9330f, -0.0300f}},  // 98: Right buttock helper: may keep her hips round when she bends
	{2609, 89, {0.0660f, 0.9330f, -0.0300f}},  // 99: Left buttock helper: may keep her hips round when she bends
	{3102, 89, {-0.0759f, 1.0871f, 0.0772f}},  // 100: Dress: a front flap on her right (flap 1, joint 1)
	{3103, 100, {-0.0669f, 1.0058f, 0.1297f}},  // 101: Dress: a front flap on her right (flap 1, joint 2)
	{3104, 101, {-0.0591f, 0.9135f, 0.1620f}},  // 102: Dress: a front flap on her right (flap 1, joint 3)
	{3105, 89, {-0.0877f, 1.0873f, 0.0751f}},  // 103: Dress: a front flap on her right (flap 2, joint 1)
	{3106, 103, {-0.1186f, 1.0113f, 0.0979f}},  // 104: Dress: a front flap on her right (flap 2, joint 2)
	{3107, 104, {-0.1413f, 0.9429f, 0.1064f}},  // 105: Dress: a front flap on her right (flap 2, joint 3)
	{2657, 89, {0.0937f, 0.9922f, 0.0648f}},  // 106: Front of the left hip: may keep the hip crease smooth
	{2656, 89, {-0.0937f, 0.9922f, 0.0648f}},  // 107: Front of the right hip: may keep the hip crease smooth
	{3184, 89, {-0.0769f, 1.0500f, 0.0843f}},  // 108: Skirt: strand 1 of 15 (front right), joint 1 of 6
	{3185, 108, {-0.0589f, 0.9700f, 0.1214f}},  // 109: Skirt: strand 1 of 15 (front right), joint 2 of 6
	{3186, 109, {-0.0427f, 0.8900f, 0.1392f}},  // 110: Skirt: strand 1 of 15 (front right), joint 3 of 6
	{3187, 110, {-0.0229f, 0.8100f, 0.1507f}},  // 111: Skirt: strand 1 of 15 (front right), joint 4 of 6
	{3188, 111, {-0.0083f, 0.7200f, 0.1611f}},  // 112: Skirt: strand 1 of 15 (front right), joint 5 of 6
	{3189, 112, {0.0118f, 0.6400f, 0.1692f}},  // 113: Skirt: strand 1 of 15 (front right), joint 6 of 6
	{3190, 89, {0.0059f, 1.0500f, 0.1068f}},  // 114: Skirt: strand 2 of 15 (front), joint 1 of 6
	{3191, 114, {0.0187f, 0.9700f, 0.1160f}},  // 115: Skirt: strand 2 of 15 (front), joint 2 of 6
	{3192, 115, {0.0353f, 0.8900f, 0.1356f}},  // 116: Skirt: strand 2 of 15 (front), joint 3 of 6
	{3193, 116, {0.0575f, 0.8100f, 0.1563f}},  // 117: Skirt: strand 2 of 15 (front), joint 4 of 6
	{3194, 117, {0.0797f, 0.7200f, 0.1808f}},  // 118: Skirt: strand 2 of 15 (front), joint 5 of 6
	{3195, 118, {0.1019f, 0.6400f, 0.2053f}},  // 119: Skirt: strand 2 of 15 (front), joint 6 of 6
	{3196, 89, {0.0742f, 1.0500f, 0.0876f}},  // 120: Skirt: strand 3 of 15 (front left), joint 1 of 6
	{3197, 120, {0.0978f, 0.9700f, 0.0917f}},  // 121: Skirt: strand 3 of 15 (front left), joint 2 of 6
	{3198, 121, {0.1255f, 0.8900f, 0.1056f}},  // 122: Skirt: strand 3 of 15 (front left), joint 3 of 6
	{3199, 122, {0.1453f, 0.8100f, 0.1197f}},  // 123: Skirt: strand 3 of 15 (front left), joint 4 of 6
	{3200, 123, {0.1668f, 0.7200f, 0.1352f}},  // 124: Skirt: strand 3 of 15 (front left), joint 5 of 6
	{3201, 124, {0.1894f, 0.6400f, 0.1516f}},  // 125: Skirt: strand 3 of 15 (front left), joint 6 of 6
	{3202, 89, {0.1206f, 1.0500f, 0.0629f}},  // 126: Skirt: strand 4 of 15 (front left), joint 1 of 6
	{3203, 126, {0.1478f, 0.9700f, 0.0623f}},  // 127: Skirt: strand 4 of 15 (front left), joint 2 of 6
	{3204, 127, {0.1687f, 0.8900f, 0.0660f}},  // 128: Skirt: strand 4 of 15 (front left), joint 3 of 6
	{3205, 128, {0.1887f, 0.8100f, 0.0747f}},  // 129: Skirt: strand 4 of 15 (front left), joint 4 of 6
	{3206, 129, {0.2167f, 0.7200f, 0.0834f}},  // 130: Skirt: strand 4 of 15 (front left), joint 5 of 6
	{3207, 130, {0.2447f, 0.6400f, 0.0921f}},  // 131: Skirt: strand 4 of 15 (front left), joint 6 of 6
	{3208, 89, {0.1205f, 1.0500f, 0.0085f}},  // 132: Skirt: strand 5 of 15 (left), joint 1 of 6
	{3209, 132, {0.1637f, 0.9700f, 0.0081f}},  // 133: Skirt: strand 5 of 15 (left), joint 2 of 6
	{3210, 133, {0.2041f, 0.8900f, 0.0054f}},  // 134: Skirt: strand 5 of 15 (left), joint 3 of 6
	{3211, 134, {0.2436f, 0.8100f, 0.0001f}},  // 135: Skirt: strand 5 of 15 (left), joint 4 of 6
	{3212, 135, {0.2877f, 0.7200f, -0.0052f}},  // 136: Skirt: strand 5 of 15 (left), joint 5 of 6
	{3213, 136, {0.3237f, 0.6400f, -0.0105f}},  // 137: Skirt: strand 5 of 15 (left), joint 6 of 6
	{3214, 89, {0.1054f, 1.0500f, -0.0373f}},  // 138: Skirt: strand 6 of 15 (left), joint 1 of 6
	{3215, 138, {0.1409f, 0.9700f, -0.0447f}},  // 139: Skirt: strand 6 of 15 (left), joint 2 of 6
	{3216, 139, {0.1720f, 0.8900f, -0.0588f}},  // 140: Skirt: strand 6 of 15 (left), joint 3 of 6
	{3217, 140, {0.2094f, 0.8100f, -0.0746f}},  // 141: Skirt: strand 6 of 15 (left), joint 4 of 6
	{3218, 141, {0.2468f, 0.7200f, -0.0904f}},  // 142: Skirt: strand 6 of 15 (left), joint 5 of 6
	{3219, 142, {0.2842f, 0.6400f, -0.1062f}},  // 143: Skirt: strand 6 of 15 (left), joint 6 of 6
	{3220, 89, {0.0768f, 1.0500f, -0.0711f}},  // 144: Skirt: strand 7 of 15 (back left), joint 1 of 6
	{3221, 144, {0.1024f, 0.9700f, -0.0880f}},  // 145: Skirt: strand 7 of 15 (back left), joint 2 of 6
	{3222, 145, {0.1442f, 0.8900f, -0.1181f}},  // 146: Skirt: strand 7 of 15 (back left), joint 3 of 6
	{3223, 146, {0.1736f, 0.8100f, -0.1419f}},  // 147: Skirt: strand 7 of 15 (back left), joint 4 of 6
	{3224, 147, {0.1967f, 0.7200f, -0.1624f}},  // 148: Skirt: strand 7 of 15 (back left), joint 5 of 6
	{3225, 148, {0.2263f, 0.6400f, -0.1920f}},  // 149: Skirt: strand 7 of 15 (back left), joint 6 of 6
	{3226, 89, {0.0360f, 1.0500f, -0.0855f}},  // 150: Skirt: strand 8 of 15 (back left), joint 1 of 6
	{3227, 150, {0.0605f, 0.9700f, -0.1242f}},  // 151: Skirt: strand 8 of 15 (back left), joint 2 of 6
	{3228, 151, {0.0818f, 0.8900f, -0.1587f}},  // 152: Skirt: strand 8 of 15 (back left), joint 3 of 6
	{3229, 152, {0.1000f, 0.8100f, -0.1952f}},  // 153: Skirt: strand 8 of 15 (back left), joint 4 of 6
	{3230, 153, {0.1182f, 0.7200f, -0.2317f}},  // 154: Skirt: strand 8 of 15 (back left), joint 5 of 6
	{3231, 154, {0.1364f, 0.6400f, -0.2682f}},  // 155: Skirt: strand 8 of 15 (back left), joint 6 of 6
	{3232, 89, {0.0013f, 1.0500f, -0.0860f}},  // 156: Skirt: strand 9 of 15 (back), joint 1 of 6
	{3233, 156, {0.0056f, 0.9700f, -0.1152f}},  // 157: Skirt: strand 9 of 15 (back), joint 2 of 6
	{3234, 157, {0.0111f, 0.8900f, -0.1618f}},  // 158: Skirt: strand 9 of 15 (back), joint 3 of 6
	{3235, 158, {0.0121f, 0.8100f, -0.1929f}},  // 159: Skirt: strand 9 of 15 (back), joint 4 of 6
	{3236, 159, {0.0135f, 0.7200f, -0.2269f}},  // 160: Skirt: strand 9 of 15 (back), joint 5 of 6
	{3237, 160, {0.0148f, 0.6400f, -0.2652f}},  // 161: Skirt: strand 9 of 15 (back), joint 6 of 6
	{3238, 89, {-0.0438f, 1.0500f, -0.0873f}},  // 162: Skirt: strand 10 of 15 (back right), joint 1 of 6
	{3239, 162, {-0.0586f, 0.9700f, -0.1265f}},  // 163: Skirt: strand 10 of 15 (back right), joint 2 of 6
	{3240, 163, {-0.0714f, 0.8900f, -0.1675f}},  // 164: Skirt: strand 10 of 15 (back right), joint 3 of 6
	{3241, 164, {-0.0807f, 0.8100f, -0.2093f}},  // 165: Skirt: strand 10 of 15 (back right), joint 4 of 6
	{3242, 165, {-0.0900f, 0.7200f, -0.2511f}},  // 166: Skirt: strand 10 of 15 (back right), joint 5 of 6
	{3243, 166, {-0.0993f, 0.6400f, -0.2929f}},  // 167: Skirt: strand 10 of 15 (back right), joint 6 of 6
	{3244, 89, {-0.0756f, 1.0500f, -0.0748f}},  // 168: Skirt: strand 11 of 15 (back right), joint 1 of 6
	{3245, 168, {-0.1021f, 0.9700f, -0.1017f}},  // 169: Skirt: strand 11 of 15 (back right), joint 2 of 6
	{3246, 169, {-0.1205f, 0.8900f, -0.1341f}},  // 170: Skirt: strand 11 of 15 (back right), joint 3 of 6
	{3247, 170, {-0.1462f, 0.8100f, -0.1726f}},  // 171: Skirt: strand 11 of 15 (back right), joint 4 of 6
	{3248, 171, {-0.1652f, 0.7200f, -0.2093f}},  // 172: Skirt: strand 11 of 15 (back right), joint 5 of 6
	{3249, 172, {-0.1836f, 0.6400f, -0.2447f}},  // 173: Skirt: strand 11 of 15 (back right), joint 6 of 6
	{3250, 89, {-0.1065f, 1.0500f, -0.0498f}},  // 174: Skirt: strand 12 of 15 (back right), joint 1 of 6
	{3251, 174, {-0.1262f, 0.9700f, -0.0663f}},  // 175: Skirt: strand 12 of 15 (back right), joint 2 of 6
	{3252, 175, {-0.1566f, 0.8900f, -0.0903f}},  // 176: Skirt: strand 12 of 15 (back right), joint 3 of 6
	{3253, 176, {-0.1837f, 0.8100f, -0.1133f}},  // 177: Skirt: strand 12 of 15 (back right), joint 4 of 6
	{3254, 177, {-0.2108f, 0.7200f, -0.1363f}},  // 178: Skirt: strand 12 of 15 (back right), joint 5 of 6
	{3255, 178, {-0.2379f, 0.6400f, -0.1593f}},  // 179: Skirt: strand 12 of 15 (back right), joint 6 of 6
	{3256, 89, {-0.1168f, 1.0500f, -0.0147f}},  // 180: Skirt: strand 13 of 15 (right), joint 1 of 6
	{3257, 180, {-0.1513f, 0.9700f, -0.0338f}},  // 181: Skirt: strand 13 of 15 (right), joint 2 of 6
	{3258, 181, {-0.1993f, 0.8900f, -0.0557f}},  // 182: Skirt: strand 13 of 15 (right), joint 3 of 6
	{3259, 182, {-0.2491f, 0.8100f, -0.0793f}},  // 183: Skirt: strand 13 of 15 (right), joint 4 of 6
	{3260, 183, {-0.2822f, 0.7200f, -0.0976f}},  // 184: Skirt: strand 13 of 15 (right), joint 5 of 6
	{3261, 184, {-0.3029f, 0.6400f, -0.1136f}},  // 185: Skirt: strand 13 of 15 (right), joint 6 of 6
	{3262, 89, {-0.1252f, 1.0500f, 0.0290f}},  // 186: Skirt: strand 14 of 15 (right), joint 1 of 6
	{3263, 186, {-0.1710f, 0.9700f, 0.0208f}},  // 187: Skirt: strand 14 of 15 (right), joint 2 of 6
	{3264, 187, {-0.2283f, 0.8900f, 0.0100f}},  // 188: Skirt: strand 14 of 15 (right), joint 3 of 6
	{3265, 188, {-0.2708f, 0.8100f, -0.0031f}},  // 189: Skirt: strand 14 of 15 (right), joint 4 of 6
	{3266, 189, {-0.3075f, 0.7200f, -0.0204f}},  // 190: Skirt: strand 14 of 15 (right), joint 5 of 6
	{3267, 190, {-0.3389f, 0.6400f, -0.0348f}},  // 191: Skirt: strand 14 of 15 (right), joint 6 of 6
	{3268, 89, {-0.1013f, 1.0500f, 0.0721f}},  // 192: Skirt: strand 15 of 15 (front right), joint 1 of 6
	{3269, 192, {-0.1520f, 0.9700f, 0.0828f}},  // 193: Skirt: strand 15 of 15 (front right), joint 2 of 6
	{3270, 193, {-0.1965f, 0.8900f, 0.0785f}},  // 194: Skirt: strand 15 of 15 (front right), joint 3 of 6
	{3271, 194, {-0.2347f, 0.8100f, 0.0793f}},  // 195: Skirt: strand 15 of 15 (front right), joint 4 of 6
	{3272, 195, {-0.2763f, 0.7200f, 0.0690f}},  // 196: Skirt: strand 15 of 15 (front right), joint 5 of 6
	{3273, 196, {-0.3074f, 0.6400f, 0.0569f}},  // 197: Skirt: strand 15 of 15 (front right), joint 6 of 6
};
inline constexpr int32_t kSkeletonBones = std::size(kSkeleton);

// The bones the first-person body works with (indices into kSkeleton).
enum SKELETON_BONE : int32_t {
	BONE_HIPS = 2,
	BONE_WAIST = 3,
	BONE_SPINE = 4,
	BONE_CHEST = 5,
	BONE_NECK = 6,
	BONE_HEAD = 7,
	BONE_RIGHT_CLAVICLE = 34,
	BONE_RIGHT_UPPER_ARM = 35,
	BONE_RIGHT_FOREARM = 36,
	BONE_RIGHT_HAND = 37,
	BONE_LEFT_CLAVICLE = 61,
	BONE_LEFT_UPPER_ARM = 62,
	BONE_LEFT_FOREARM = 63,
	BONE_LEFT_HAND = 64,
	BONE_PELVIS = 89,
	BONE_RIGHT_THIGH = 90,
	BONE_RIGHT_SHIN = 91,
	BONE_RIGHT_ANKLE = 92,
	BONE_RIGHT_TOES = 93,
	BONE_LEFT_THIGH = 94,
	BONE_LEFT_SHIN = 95,
	BONE_LEFT_ANKLE = 96,
	BONE_LEFT_TOES = 97,
};

// Each of her parts' bone list (kBodyPartBones order): the bone (kSkeleton index) in each slot.
inline constexpr uint8_t kPart0Slots[] = {94, 90, 34, 5, 6, 33, 61, 88, 4, 3, 7, 35, 62, 63, 36, 58, 60, 85, 87, 53, 54, 49, 50, 37, 55, 56, 51, 45, 46, 52, 47, 42, 41, 48, 43, 44, 39, 38, 40, 65, 66, 69, 70, 64, 73, 74, 77, 81, 78, 82, 83, 79, 80, 75, 76, 71, 72, 67, 68};
inline constexpr uint8_t kPart1Slots[] = {7};
inline constexpr uint8_t kPart2Slots[] = {116, 115, 117, 110, 114, 122, 121, 120, 89, 103, 100, 104, 101, 105, 102};
inline constexpr uint8_t kPart3Slots[] = {3, 89, 4, 132, 121, 120, 127, 126, 133, 139, 138, 145, 144, 151, 150, 157, 156, 163, 162, 169, 168, 175, 186, 174, 181, 180, 187, 188, 182, 177, 176, 178, 183, 171, 170, 172, 164, 165, 158, 159, 160, 153, 152, 146, 147, 140, 141, 134, 135, 142, 136, 129, 130, 128, 122, 123, 124, 117, 116, 118, 125, 115, 110};
inline constexpr uint8_t kPart4Slots[] = {4, 3, 89, 186, 193, 187, 109, 108, 192, 181, 194, 195, 188, 196, 189, 190, 197, 183, 184, 182, 177, 178, 176, 171, 172, 170, 165, 164, 163, 158, 159, 160, 153, 154, 152, 151, 147, 146, 148, 141, 142, 140, 135, 139, 136, 129, 130, 128, 123, 124, 125, 118, 117, 116, 111, 112, 113, 110, 115, 114, 121, 120, 127};
inline constexpr uint8_t kPart5Slots[] = {53, 54, 37, 49, 57, 45, 38, 41, 36, 42, 58, 65, 66, 64, 69, 84, 73, 63, 85, 77, 81, 78, 89, 3, 4};
inline constexpr uint8_t kPart6Slots[] = {164, 165, 158, 159, 163, 170, 171, 177, 176, 178, 175, 183, 182, 187, 188, 186, 181, 180, 89, 3, 4, 5, 174, 2, 109, 108, 169, 114, 156, 115, 121, 120, 150, 132, 116, 117, 122, 123, 124, 129, 134, 140, 133, 139, 135, 141, 128, 146, 147, 145, 152, 153, 151, 157, 160, 162, 168, 172, 138, 127, 126, 144};
inline constexpr uint8_t kPart7Slots[] = {171, 172, 165, 170, 164, 177, 178, 176, 183, 182, 184, 189, 190, 188, 195, 194, 196, 193, 187, 192, 186, 89, 181, 3, 4, 5, 109, 108, 2, 115, 114, 110, 116, 111, 117, 112, 118, 113, 123, 124, 122, 125, 130, 129, 128, 127, 135, 134, 136, 141, 140, 142, 139, 146, 147, 148, 153, 152, 151, 159, 158, 154, 160};
inline constexpr uint8_t kPart8Slots[] = {90, 94};
inline constexpr uint8_t kPart9Slots[] = {30, 27, 29, 28, 7, 31, 14, 15, 13, 12, 23, 24, 21, 17, 16, 18, 19, 25, 22, 32, 26, 11, 20};
inline constexpr uint8_t kPart10Slots[] = {14, 13, 12, 15, 7, 21, 20, 23, 31, 26, 25, 32, 29, 30, 18, 19, 17, 16, 27, 28};
inline constexpr uint8_t kPart11Slots[] = {63, 86, 85, 64, 81, 62, 84, 36, 59, 58, 37, 38, 35, 57};
inline constexpr uint8_t kPart12Slots[] = {5, 4, 34, 88, 61, 89, 90, 98, 3, 94, 99};
inline constexpr uint8_t kPart13Slots[] = {8, 7, 6, 11, 10, 9, 33, 15, 14, 18, 19, 5, 34, 61};
inline constexpr uint8_t kPart14Slots[] = {92, 91, 90, 93, 96, 95, 97, 94};
// Part 13 (her face) with her serious face, in combat: the same bones in another order.
inline constexpr uint8_t kPart13SeriousSlots[] = {7, 8, 6, 11, 9, 10, 33, 5, 61, 34, 15, 14, 18, 19};

struct PartSlots {
	const uint8_t* bones;
	int32_t count;
};
inline constexpr PartSlots kPartSlots[] = {
	{kPart0Slots, std::size(kPart0Slots)},
	{kPart1Slots, std::size(kPart1Slots)},
	{kPart2Slots, std::size(kPart2Slots)},
	{kPart3Slots, std::size(kPart3Slots)},
	{kPart4Slots, std::size(kPart4Slots)},
	{kPart5Slots, std::size(kPart5Slots)},
	{kPart6Slots, std::size(kPart6Slots)},
	{kPart7Slots, std::size(kPart7Slots)},
	{kPart8Slots, std::size(kPart8Slots)},
	{kPart9Slots, std::size(kPart9Slots)},
	{kPart10Slots, std::size(kPart10Slots)},
	{kPart11Slots, std::size(kPart11Slots)},
	{kPart12Slots, std::size(kPart12Slots)},
	{kPart13Slots, std::size(kPart13Slots)},
	{kPart14Slots, std::size(kPart14Slots)},
};
// clang-format on

}  // namespace pure
