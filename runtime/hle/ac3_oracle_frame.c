#include "../../recomp/main/ac3_funcs.h"

void ac3_oracle_frame_snap(ctx_t *ctx, u32 target, u32 checkpoint);

static u32 frame_phase;

static void flight_frame_stepped(ctx_t *ctx) {
    ctx->r[29] = ctx->r[29] + 0xFFFFFFE0u;
    MEM_W32(ctx->r[29] + 0x00000010u, ctx->r[16]);
    ctx->r[16] = ctx->r[4] + 0u;
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[5] = 0u + 0x00001000u;
    MEM_W32(ctx->r[29] + 0x0000001Cu, ctx->r[31]);
    MEM_W32(ctx->r[29] + 0x00000018u, ctx->r[18]);
    MEM_W32(ctx->r[29] + 0x00000014u, ctx->r[17]);
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000004u);
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[2] = SHL(ctx->r[2], 2u);
    ctx->r[2] = ctx->r[3] + ctx->r[2];
    ctx->r[4] = MEM_R32(ctx->r[2] + 0x00000018u);
    ctx->r[17] = 0x00FF0000u;
    MEM_W32(ctx->r[3] + 0x00000010u, ctx->r[4]);
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000020u);
    ctx->r[17] = ctx->r[17] | 0xFFFFu;
    {
        ctx->r[31] = 0x80033A2Cu;
        MEM_W32(ctx->r[3] + 0x00000014u, ctx->r[2]);
        psyq_ClearOTagR(ctx);
    }
    ctx->r[18] = 0xFF000000u;
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[6] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[5] = MEM_R32(ctx->r[4] + 0x00000010u);
    ctx->r[2] = MEM_R32(ctx->r[6] + 0x00000008u);
    ctx->r[3] = MEM_R32(ctx->r[5] + 0x00000000u);
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000000u);
    ctx->r[3] = ctx->r[3] & ctx->r[18];
    ctx->r[2] = ctx->r[2] & ctx->r[17];
    ctx->r[3] = ctx->r[3] | ctx->r[2];
    MEM_W32(ctx->r[5] + 0x00000000u, ctx->r[3]);
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000010u);
    ctx->r[5] = MEM_R32(ctx->r[6] + 0x00000008u);
    ctx->r[2] = ctx->r[2] + 0x00003FFCu;
    ctx->r[3] = MEM_R32(ctx->r[5] + 0x00000000u);
    ctx->r[2] = ctx->r[2] & ctx->r[17];
    ctx->r[3] = ctx->r[3] & ctx->r[18];
    ctx->r[3] = ctx->r[3] | ctx->r[2];
    MEM_W32(ctx->r[5] + 0x00000000u, ctx->r[3]);
    MEM_W16(ctx->r[4] + 0x0000028Cu, 0u);
    MEM_W16(ctx->r[4] + 0x0000028Eu, 0u);
    MEM_W16(ctx->r[4] + 0x00000290u, 0u);
    { u32 _t = ctx->r[16];
        ctx->r[31] = 0x80033A8Cu;
        MEM_W16(ctx->r[4] + 0x00000292u, 0u);
        ac3_call_indirect(ctx, _t);
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R32(ctx->r[2] + 0x00000028u);
    {
        ctx->r[31] = 0x80033AA0u;
        func_8007B438(ctx);
    }
    ctx->r[6] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[5] = ctx->r[6] + 0x0000002Cu;
    ctx->r[4] = MEM_R32(ctx->r[6] + 0x00000028u);
    {
        ctx->r[31] = 0x80033AB8u;
        ctx->r[6] = ctx->r[6] + 0x0000004Cu;
        func_800332E4(ctx);
    }
    ac3_oracle_frame_snap(ctx, frame_phase, 18u);
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[3] = MEM_R32(ctx->r[2] + 0x00000004u);
    ctx->r[5] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[2] = SHL(ctx->r[3], 8u);
    ctx->r[2] = ctx->r[2] + ctx->r[3];
    ctx->r[2] = SHL(ctx->r[2], 2u);
    ctx->r[2] = ctx->r[2] + ctx->r[3];
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E4u);
    ctx->r[2] = SHL(ctx->r[2], 2u);
    ctx->r[3] = ctx->r[3] + ctx->r[2];
    ctx->r[2] = MEM_R32(ctx->r[3] + 0x00000000u);
    ctx->r[7] = MEM_R32(ctx->r[5] + 0x00000014u);
    ctx->r[9] = MEM_R16S(ctx->r[2] + 0x00000008u);
    ctx->r[8] = MEM_R16S(ctx->r[2] + 0x0000000Au);
    ctx->r[6] = MEM_R16U(ctx->r[2] + 0x00000004u);
    ctx->r[4] = MEM_R16U(ctx->r[2] + 0x00000006u);
    ctx->r[2] = MEM_R16U(ctx->r[5] + 0x0000028Cu);
    ctx->r[3] = MEM_R16U(ctx->r[5] + 0x0000028Eu);
    ctx->r[2] = ctx->r[2] + ctx->r[9];
    ctx->r[3] = ctx->r[3] + ctx->r[8];
    ctx->r[6] = SHL(ctx->r[6], 16u);
    MEM_W16(ctx->r[5] + 0x00000294u, ctx->r[2]);
    ctx->r[2] = SAR(ctx->r[6], 17u);
    ctx->r[2] = ctx->r[9] + ctx->r[2];
    ctx->r[4] = SHL(ctx->r[4], 16u);
    MEM_W16(ctx->r[5] + 0x00000296u, ctx->r[3]);
    ctx->r[3] = MEM_R16U(ctx->r[5] + 0x00000290u);
    ctx->r[4] = SAR(ctx->r[4], 17u);
    ctx->r[3] = ctx->r[3] + ctx->r[2];
    ctx->r[2] = MEM_R16U(ctx->r[5] + 0x00000292u);
    ctx->r[4] = ctx->r[8] + ctx->r[4];
    ctx->r[2] = ctx->r[2] + ctx->r[4];
    MEM_W16(ctx->r[5] + 0x0000029Au, ctx->r[2]);
    ctx->r[2] = MEM_R16S(ctx->r[5] + 0x00000290u);
    ctx->r[6] = SAR(ctx->r[6], 16u);
    { u32 _c = (ctx->r[2] == 0u);
        MEM_W16(ctx->r[5] + 0x00000298u, ctx->r[3]);
        if (_c)
            goto L_80033BC0;
    }
    ctx->r[2] = 0u + 0x00000003u;
    MEM_W8(ctx->r[7] + 0x00000003u, ctx->r[2]);
    ctx->r[2] = 0u + 0x00000060u;
    MEM_W8(ctx->r[7] + 0x00000007u, ctx->r[2]);
    MEM_W8(ctx->r[7] + 0x00000004u, 0u);
    MEM_W8(ctx->r[7] + 0x00000005u, 0u);
    MEM_W8(ctx->r[7] + 0x00000006u, 0u);
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002E0u);
    MEM_W16(ctx->r[7] + 0x00000008u, 0u);
    MEM_W16(ctx->r[7] + 0x0000000Au, 0u);
    ctx->r[3] = MEM_R16U(ctx->r[4] + 0x00000290u);
    ctx->r[2] = 0u + 0x000000F0u;
    MEM_W16(ctx->r[7] + 0x0000000Eu, ctx->r[2]);
    MEM_W16(ctx->r[7] + 0x0000000Cu, ctx->r[3]);
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000010u);
    ctx->r[3] = MEM_R32(ctx->r[7] + 0x00000000u);
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000000u);
    ctx->r[3] = ctx->r[3] & ctx->r[18];
    ctx->r[2] = ctx->r[2] & ctx->r[17];
    ctx->r[3] = ctx->r[3] | ctx->r[2];
    MEM_W32(ctx->r[7] + 0x00000000u, ctx->r[3]);
    ctx->r[3] = MEM_R32(ctx->r[4] + 0x00000010u);
    ctx->r[4] = ctx->r[7] & ctx->r[17];
    ctx->r[2] = MEM_R32(ctx->r[3] + 0x00000000u);
    ctx->r[7] = ctx->r[7] + 0x00000010u;
    ctx->r[2] = ctx->r[2] & ctx->r[18];
    ctx->r[2] = ctx->r[2] | ctx->r[4];
    MEM_W32(ctx->r[3] + 0x00000000u, ctx->r[2]);
L_80033BC0:;
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[2] = MEM_R16S(ctx->r[2] + 0x00000292u);
    { u32 _c = (ctx->r[2] == 0u);
        ctx->r[2] = 0u + 0x00000003u;
        if (_c)
            goto L_80033C44;
    }
    MEM_W8(ctx->r[7] + 0x00000003u, ctx->r[2]);
    ctx->r[2] = 0u + 0x00000060u;
    MEM_W8(ctx->r[7] + 0x00000007u, ctx->r[2]);
    MEM_W8(ctx->r[7] + 0x00000004u, 0u);
    MEM_W8(ctx->r[7] + 0x00000005u, 0u);
    MEM_W8(ctx->r[7] + 0x00000006u, 0u);
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002E0u);
    MEM_W16(ctx->r[7] + 0x00000008u, 0u);
    MEM_W16(ctx->r[7] + 0x0000000Au, 0u);
    MEM_W16(ctx->r[7] + 0x0000000Cu, ctx->r[6]);
    ctx->r[2] = MEM_R16U(ctx->r[4] + 0x00000292u);
    MEM_W16(ctx->r[7] + 0x0000000Eu, ctx->r[2]);
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000010u);
    ctx->r[3] = MEM_R32(ctx->r[7] + 0x00000000u);
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000000u);
    ctx->r[3] = ctx->r[3] & ctx->r[18];
    ctx->r[2] = ctx->r[2] & ctx->r[17];
    ctx->r[3] = ctx->r[3] | ctx->r[2];
    MEM_W32(ctx->r[7] + 0x00000000u, ctx->r[3]);
    ctx->r[3] = MEM_R32(ctx->r[4] + 0x00000010u);
    ctx->r[4] = ctx->r[7] & ctx->r[17];
    ctx->r[2] = MEM_R32(ctx->r[3] + 0x00000000u);
    ctx->r[7] = ctx->r[7] + 0x00000010u;
    ctx->r[2] = ctx->r[2] & ctx->r[18];
    ctx->r[2] = ctx->r[2] | ctx->r[4];
    MEM_W32(ctx->r[3] + 0x00000000u, ctx->r[2]);
L_80033C44:;
    ctx->r[2] = SHL(ctx->r[8], 11u);
    ctx->r[2] = ctx->r[2] | ctx->r[9];
    ctx->r[16] = 0xE5000000u;
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[2] = ctx->r[2] | ctx->r[16];
    MEM_W32(ctx->r[3] + 0x00000014u, ctx->r[7]);
    MEM_W32(ctx->r[7] + 0x00000004u, ctx->r[2]);
    MEM_W32(ctx->r[7] + 0x00000008u, 0u);
    ctx->r[2] = MEM_R32(ctx->r[3] + 0x00000010u);
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000000u);
    ctx->r[17] = 0x02000000u;
    ctx->r[2] = ctx->r[2] | ctx->r[17];
    MEM_W32(ctx->r[7] + 0x00000000u, ctx->r[2]);
    ctx->r[2] = SHL(ctx->r[7], 8u);
    ctx->r[3] = MEM_R32(ctx->r[3] + 0x00000010u);
    ctx->r[2] = SHR(ctx->r[2], 8u);
    MEM_W32(ctx->r[3] + 0x00000000u, ctx->r[2]);
    ctx->r[6] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R16S(ctx->r[6] + 0x00000294u);
    ctx->r[2] = MEM_R16S(ctx->r[6] + 0x00000296u);
    ctx->r[3] = ctx->r[7] + 0x0000000Cu;
    MEM_W32(ctx->r[6] + 0x00000014u, ctx->r[3]);
    MEM_W32(ctx->r[3] + 0x00000008u, 0u);
    ctx->r[2] = SHL(ctx->r[2], 11u);
    ctx->r[2] = ctx->r[2] | ctx->r[4];
    ctx->r[2] = ctx->r[2] | ctx->r[16];
    ctx->r[4] = 0u + 0u;
    MEM_W32(ctx->r[3] + 0x00000004u, ctx->r[2]);
    ctx->r[2] = MEM_R32(ctx->r[6] + 0x00000010u);
    ctx->r[5] = ctx->r[4] + 0u;
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000004u);
    ctx->r[3] = SHL(ctx->r[3], 8u);
    ctx->r[2] = ctx->r[2] | ctx->r[17];
    MEM_W32(ctx->r[7] + 0x0000000Cu, ctx->r[2]);
    ctx->r[2] = MEM_R32(ctx->r[6] + 0x00000010u);
    ctx->r[3] = SHR(ctx->r[3], 8u);
    MEM_W32(ctx->r[2] + 0x00000004u, ctx->r[3]);
    ctx->r[2] = ctx->r[7] + 0x00000018u;
    {
        ctx->r[31] = 0x80033CECu;
        MEM_W32(ctx->r[6] + 0x00000014u, ctx->r[2]);
        psyq_SetGeomOffset(ctx);
    }
    {
        ctx->r[31] = 0x80033CF4u;
        func_80033324(ctx);
    }
    {
        ctx->r[31] = 0x80033CFCu;
        func_800333D8(ctx);
    }
    {
        ctx->r[31] = 0x80033D04u;
        func_8003342C(ctx);
    }
    {
        ctx->r[31] = 0x80033D0Cu;
        func_8003346C(ctx);
    }
    ctx->r[7] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[3] = MEM_R32(ctx->r[7] + 0x00000014u);
    ctx->r[2] = MEM_R16S(ctx->r[7] + 0x0000029Au);
    ctx->r[4] = MEM_R16S(ctx->r[7] + 0x00000298u);
    ctx->r[2] = SHL(ctx->r[2], 11u);
    ctx->r[2] = ctx->r[2] | ctx->r[4];
    ctx->r[2] = ctx->r[2] | ctx->r[16];
    MEM_W32(ctx->r[3] + 0x00000004u, ctx->r[2]);
    MEM_W32(ctx->r[3] + 0x00000008u, 0u);
    ctx->r[2] = MEM_R32(ctx->r[7] + 0x00000010u);
    ctx->r[5] = 0u + 0x00000078u;
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00003FFCu);
    ctx->r[4] = 0u + 0x000000A0u;
    ctx->r[2] = ctx->r[2] | ctx->r[17];
    MEM_W32(ctx->r[3] + 0x00000000u, ctx->r[2]);
    ctx->r[2] = SHL(ctx->r[3], 8u);
    ctx->r[3] = ctx->r[3] + 0x0000000Cu;
    ctx->r[6] = MEM_R32(ctx->r[7] + 0x00000010u);
    ctx->r[2] = SHR(ctx->r[2], 8u);
    MEM_W32(ctx->r[6] + 0x00003FFCu, ctx->r[2]);
    {
        ctx->r[31] = 0x80033D68u;
        MEM_W32(ctx->r[7] + 0x00000014u, ctx->r[3]);
        psyq_SetGeomOffset(ctx);
    }
    {
        ctx->r[31] = 0x80033D70u;
        func_8003A2B8(ctx);
    }
    {
        ctx->r[31] = 0x80033D78u;
        func_800337BC(ctx);
    }
    {
        ctx->r[31] = 0x80033D80u;
        func_800337DC(ctx);
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000000u);
    ctx->r[2] = ~(0u | ctx->r[2]);
    ctx->r[2] = ctx->r[2] & 0x0001u;
    { u32 _c = (ctx->r[2] == 0u);
        if (_c)
            goto L_80033DA8;
    }
    {
        ctx->r[31] = 0x80033DA8u;
        func_80033814(ctx);
    }
L_80033DA8:;
    ac3_oracle_frame_snap(ctx, frame_phase, 19u);
    ctx->r[31] = MEM_R32(ctx->r[29] + 0x0000001Cu);
    ctx->r[18] = MEM_R32(ctx->r[29] + 0x00000018u);
    ctx->r[17] = MEM_R32(ctx->r[29] + 0x00000014u);
    ctx->r[16] = MEM_R32(ctx->r[29] + 0x00000010u);
    {
        ctx->r[29] = ctx->r[29] + 0x00000020u;
        return;
    }
}

static void phase_80033EF0_stepped(ctx_t *ctx) {
    frame_phase = 0x80033EF0u;
    ac3_oracle_frame_snap(ctx, 0x80033EF0u, 16u);
    ctx->r[29] = ctx->r[29] + 0xFFFFFFE8u;
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = 0x80030000u;
    MEM_W32(ctx->r[29] + 0x00000010u, ctx->r[31]);
    ctx->r[2] = MEM_R32(ctx->r[3] + 0x00000000u);
    ctx->r[4] = ctx->r[4] + 0x00002F3Cu;
    ctx->r[2] = ctx->r[2] | 0x0002u;
    {
        ctx->r[31] = 0x80033F14u;
        MEM_W32(ctx->r[3] + 0x00000000u, ctx->r[2]);
        flight_frame_stepped(ctx);
    }
    ctx->r[5] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[3] = MEM_R32(ctx->r[5] + 0x00000000u);
    ctx->r[2] = 0x20000000u;
    ctx->r[2] = ctx->r[3] & ctx->r[2];
    { u32 _c = (ctx->r[2] == 0u);
        ctx->r[2] = ~(0u | ctx->r[3]);
        if (_c)
            goto L_80033F78;
    }
    {
        ctx->r[31] = 0x80033F38u;
        func_80034718(ctx);
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R32(ctx->r[2] + 0x0000000Cu);
    ctx->r[5] = MEM_R32(ctx->r[3] + 0x00000014u);
    {
        ctx->r[31] = 0x80033F50u;
        func_80034930(ctx);
    }
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002ECu);
    MEM_W32(ctx->r[3] + 0x00000014u, ctx->r[2]);
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000000u);
    ctx->r[3] = 0x08000000u;
    ctx->r[2] = ctx->r[2] & ctx->r[3];
    { u32 _c = (ctx->r[2] == 0u);
        ctx->r[4] = 0u + 0u;
        if (_c)
            goto L_80033FE8;
    }
    {
        goto L_80033FE0;
    }
L_80033F78:;
    ctx->r[3] = 0x10000000u;
    ctx->r[2] = ctx->r[2] & ctx->r[3];
    { u32 _c = (ctx->r[2] == 0u);
        if (_c)
            goto L_80033FE8;
    }
    ctx->r[4] = MEM_R16U(ctx->r[5] + 0x00000156u);
    ctx->r[2] = ctx->r[4] & 0x0001u;
    { u32 _c = (ctx->r[2] == 0u);
        ctx->r[3] = 0u + 0u;
        if (_c)
            goto L_80033FA8;
    }
    ctx->r[2] = ctx->r[4] & 0xFFFEu;
    MEM_W16(ctx->r[5] + 0x00000156u, ctx->r[2]);
    ctx->r[3] = 0u + 0x00000001u;
L_80033FA8:;
    ctx->r[5] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[2] = MEM_R32(ctx->r[5] + 0x0000012Cu);
    ctx->r[2] = ctx->r[2] & 0x0040u;
    { u32 _c = (ctx->r[2] == 0u);
        if (_c)
            goto L_80033FC8;
    }
    ctx->r[3] = 0u + 0x00000001u;
L_80033FC8:;
    { u32 _c = (ctx->r[3] == 0u);
        ctx->r[4] = 0u + 0x00000001u;
        if (_c)
            goto L_80033FE8;
    }
    ctx->r[2] = MEM_R32(ctx->r[5] + 0x00000000u);
    ctx->r[3] = 0x04000000u;
    ctx->r[2] = ctx->r[2] | ctx->r[3];
    MEM_W32(ctx->r[5] + 0x00000000u, ctx->r[2]);
L_80033FE0:;
    {
        ctx->r[31] = 0x80033FE8u;
        func_8002C6F4(ctx);
    }
L_80033FE8:;
    {
        ctx->r[31] = 0x80033FF0u;
        func_80033DC0(ctx);
    }
    ac3_oracle_frame_snap(ctx, 0x80033EF0u, 20u);
    ctx->r[31] = MEM_R32(ctx->r[29] + 0x00000010u);
    {
        ctx->r[29] = ctx->r[29] + 0x00000018u;
        return;
    }
}

static void phase_80034000_stepped(ctx_t *ctx) {
    frame_phase = 0x80034000u;
    ac3_oracle_frame_snap(ctx, 0x80034000u, 16u);
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[29] = ctx->r[29] + 0xFFFFFFE8u;
    MEM_W32(ctx->r[29] + 0x00000010u, ctx->r[31]);
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000000u);
    ctx->r[3] = 0u + 0xFFFFFFFDu;
    ctx->r[2] = ctx->r[2] & ctx->r[3];
    MEM_W32(ctx->r[4] + 0x00000000u, ctx->r[2]);
    ctx->r[4] = 0x80030000u;
    {
        ctx->r[31] = 0x80034028u;
        ctx->r[4] = ctx->r[4] + 0x0000307Cu;
        flight_frame_stepped(ctx);
    }
    {
        ctx->r[31] = 0x80034030u;
        func_80033DC0(ctx);
    }
    ac3_oracle_frame_snap(ctx, 0x80034000u, 20u);
    ctx->r[31] = MEM_R32(ctx->r[29] + 0x00000010u);
    {
        ctx->r[29] = ctx->r[29] + 0x00000018u;
        return;
    }
}

static void phase_80034040_stepped(ctx_t *ctx) {
    frame_phase = 0x80034040u;
    ac3_oracle_frame_snap(ctx, 0x80034040u, 16u);
    ctx->r[29] = ctx->r[29] + 0xFFFFFFE8u;
    ctx->r[4] = 0x80030000u;
    ctx->r[5] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = ctx->r[4] + 0x000030CCu;
    MEM_W32(ctx->r[29] + 0x00000010u, ctx->r[31]);
    ctx->r[2] = MEM_R32(ctx->r[5] + 0x00000000u);
    ctx->r[3] = 0u + 0xFFFFFFFDu;
    ctx->r[2] = ctx->r[2] & ctx->r[3];
    {
        ctx->r[31] = 0x80034068u;
        MEM_W32(ctx->r[5] + 0x00000000u, ctx->r[2]);
        flight_frame_stepped(ctx);
    }
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[2] = MEM_R8U(ctx->r[3] + 0x000003A5u);
    { u32 _c = (ctx->r[2] == 0u);
        if (_c)
            goto L_80034118;
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R32(ctx->r[3] + 0x0000000Cu);
    ctx->r[5] = MEM_R32(ctx->r[2] + 0x00000014u);
    {
        ctx->r[31] = 0x80034094u;
        ctx->r[6] = 0u + 0x00001000u;
        func_80035578(ctx);
    }
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    MEM_W32(ctx->r[3] + 0x00000014u, ctx->r[2]);
    ctx->r[2] = MEM_R32(ctx->r[3] + 0x00000000u);
    ctx->r[3] = 0x10000000u;
    ctx->r[2] = ctx->r[2] & ctx->r[3];
    { u32 _c = (ctx->r[2] == 0u);
        if (_c)
            goto L_80034148;
    }
    {
        ctx->r[31] = 0x800340BCu;
        func_8003048C(ctx);
    }
    ctx->r[4] = 0u + 0u;
    {
        ctx->r[31] = 0x800340C8u;
        ctx->r[5] = ctx->r[4] + 0u;
        func_8002C6A8(ctx);
    }
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[3] = MEM_R8U(ctx->r[4] + 0x000003A5u);
    ctx->r[2] = 0u + 0x00000001u;
    { u32 _c = (ctx->r[3] == ctx->r[2]);
        if (_c)
            goto L_800340F4;
    }
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000128u);
    ctx->r[2] = ctx->r[2] & 0x0010u;
    { u32 _c = (ctx->r[2] == 0u);
        if (_c)
            goto L_80034110;
    }
L_800340F4:;
    MEM_W8(ctx->r[4] + 0x000003A5u, 0u);
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[3] = MEM_R32(ctx->r[2] + 0x000003A0u);
    ctx->r[2] = 0x60000000u;
    {
        ctx->r[2] = ctx->r[3] | ctx->r[2];
        goto L_80034150;
    }
L_80034110:;
    {
        ctx->r[2] = 0x50000000u;
        goto L_80034150;
    }
L_80034118:;
    ctx->r[2] = MEM_R8U(ctx->r[3] + 0x0000039Eu);
    { u32 _c = (ctx->r[2] != 0u);
        if (_c)
            goto L_80034148;
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R32(ctx->r[3] + 0x0000000Cu);
    ctx->r[5] = MEM_R32(ctx->r[2] + 0x00000014u);
    {
        ctx->r[31] = 0x8003413Cu;
        func_8003545C(ctx);
    }
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    MEM_W32(ctx->r[3] + 0x00000014u, ctx->r[2]);
L_80034148:;
    {
        ctx->r[31] = 0x80034150u;
        func_80033DC0(ctx);
    }
L_80034150:;
    ac3_oracle_frame_snap(ctx, 0x80034040u, 20u);
    ctx->r[31] = MEM_R32(ctx->r[29] + 0x00000010u);
    {
        ctx->r[29] = ctx->r[29] + 0x00000018u;
        return;
    }
}

static void phase_80034160_stepped(ctx_t *ctx) {
    frame_phase = 0x80034160u;
    ac3_oracle_frame_snap(ctx, 0x80034160u, 16u);
    ctx->r[29] = ctx->r[29] + 0xFFFFFFE8u;
    ctx->r[4] = 0x80030000u;
    ctx->r[5] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = ctx->r[4] + 0x00003104u;
    MEM_W32(ctx->r[29] + 0x00000010u, ctx->r[31]);
    ctx->r[2] = MEM_R32(ctx->r[5] + 0x00000000u);
    ctx->r[3] = 0u + 0xFFFFFFFDu;
    ctx->r[2] = ctx->r[2] & ctx->r[3];
    {
        ctx->r[31] = 0x80034188u;
        MEM_W32(ctx->r[5] + 0x00000000u, ctx->r[2]);
        flight_frame_stepped(ctx);
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[2] = MEM_R32(ctx->r[2] + 0x00000004u);
    ctx->r[2] = ctx->r[2] & 0x1000u;
    { u32 _c = (ctx->r[2] == 0u);
        if (_c)
            goto L_80034224;
    }
    {
        ctx->r[31] = 0x800341ACu;
        func_8003514C(ctx);
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R32(ctx->r[2] + 0x0000000Cu);
    ctx->r[5] = MEM_R32(ctx->r[3] + 0x00000014u);
    {
        ctx->r[31] = 0x800341C4u;
        func_800353E8(ctx);
    }
    ctx->r[5] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002ECu);
    MEM_W32(ctx->r[5] + 0x00000014u, ctx->r[2]);
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000000u);
    ctx->r[3] = 0x02000000u;
    ctx->r[2] = ctx->r[2] | ctx->r[3];
    MEM_W32(ctx->r[4] + 0x00000000u, ctx->r[2]);
    ctx->r[3] = MEM_R32(ctx->r[5] + 0x00000000u);
    ctx->r[2] = ctx->r[3] & 0x0020u;
    { u32 _c = (ctx->r[2] == 0u);
        ctx->r[2] = 0u + 0xFFFFFFDFu;
        if (_c)
            goto L_80034224;
    }
    ctx->r[2] = ctx->r[3] & ctx->r[2];
    {
        ctx->r[31] = 0x80034200u;
        MEM_W32(ctx->r[5] + 0x00000000u, ctx->r[2]);
        func_8002DDE0(ctx);
    }
    {
        ctx->r[31] = 0x80034208u;
        func_8002937C(ctx);
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[5] = MEM_R32(ctx->r[2] + 0x00000008u);
    {
        ctx->r[31] = 0x8003421Cu;
        ctx->r[4] = 0u + 0x00000400u;
        func_80032F18(ctx);
    }
    {
        ctx->r[31] = 0x80034224u;
        func_8006D078(ctx);
    }
L_80034224:;
    {
        ctx->r[31] = 0x8003422Cu;
        func_80033DC0(ctx);
    }
    ac3_oracle_frame_snap(ctx, 0x80034160u, 20u);
    ctx->r[31] = MEM_R32(ctx->r[29] + 0x00000010u);
    {
        ctx->r[29] = ctx->r[29] + 0x00000018u;
        return;
    }
}

static void phase_800342AC_stepped(ctx_t *ctx) {
    frame_phase = 0x800342ACu;
    ac3_oracle_frame_snap(ctx, 0x800342ACu, 16u);
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[29] = ctx->r[29] + 0xFFFFFFE8u;
    MEM_W32(ctx->r[29] + 0x00000010u, ctx->r[31]);
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000000u);
    ctx->r[3] = 0u + 0xFFFFFFFDu;
    ctx->r[2] = ctx->r[2] & ctx->r[3];
    MEM_W32(ctx->r[4] + 0x00000000u, ctx->r[2]);
    ctx->r[4] = 0x80030000u;
    {
        ctx->r[31] = 0x800342D4u;
        ctx->r[4] = ctx->r[4] + 0x000031E0u;
        flight_frame_stepped(ctx);
    }
    {
        ctx->r[31] = 0x800342DCu;
        func_80033DC0(ctx);
    }
    ac3_oracle_frame_snap(ctx, 0x800342ACu, 20u);
    ctx->r[31] = MEM_R32(ctx->r[29] + 0x00000010u);
    {
        ctx->r[29] = ctx->r[29] + 0x00000018u;
        return;
    }
}

static void phase_800342EC_stepped(ctx_t *ctx) {
    frame_phase = 0x800342ECu;
    ac3_oracle_frame_snap(ctx, 0x800342ECu, 16u);
    ctx->r[29] = ctx->r[29] + 0xFFFFFFE8u;
    ctx->r[4] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[3] = 0x02000000u;
    MEM_W32(ctx->r[29] + 0x00000010u, ctx->r[31]);
    ctx->r[2] = MEM_R32(ctx->r[4] + 0x00000000u);
    ctx->r[5] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[2] = ctx->r[2] | ctx->r[3];
    MEM_W32(ctx->r[4] + 0x00000000u, ctx->r[2]);
    ctx->r[2] = MEM_R32(ctx->r[5] + 0x00000000u);
    ctx->r[3] = 0u + 0xFFFFFFFDu;
    ctx->r[2] = ctx->r[2] & ctx->r[3];
    {
        ctx->r[31] = 0x80034320u;
        MEM_W32(ctx->r[5] + 0x00000000u, ctx->r[2]);
        func_80035044(ctx);
    }
    ctx->r[4] = 0x80030000u;
    {
        ctx->r[31] = 0x8003432Cu;
        ctx->r[4] = ctx->r[4] + 0x0000329Cu;
        flight_frame_stepped(ctx);
    }
    ctx->r[2] = MEM_R32(ctx->r[28] + 0x000002ECu);
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    ctx->r[4] = MEM_R32(ctx->r[2] + 0x0000000Cu);
    ctx->r[5] = MEM_R32(ctx->r[3] + 0x00000014u);
    {
        ctx->r[31] = 0x80034344u;
        func_80035184(ctx);
    }
    ctx->r[3] = MEM_R32(ctx->r[28] + 0x000002E0u);
    {
        ctx->r[31] = 0x80034350u;
        MEM_W32(ctx->r[3] + 0x00000014u, ctx->r[2]);
        func_80033DC0(ctx);
    }
    ac3_oracle_frame_snap(ctx, 0x800342ECu, 20u);
    ctx->r[31] = MEM_R32(ctx->r[29] + 0x00000010u);
    {
        ctx->r[29] = ctx->r[29] + 0x00000018u;
        return;
    }
}

int ac3_oracle_phase_stepped(ctx_t *ctx, u32 target) {
    switch (target) {
    case 0x80033EF0u: phase_80033EF0_stepped(ctx); return 1;
    case 0x80034000u: phase_80034000_stepped(ctx); return 1;
    case 0x80034040u: phase_80034040_stepped(ctx); return 1;
    case 0x80034160u: phase_80034160_stepped(ctx); return 1;
    case 0x800342ACu: phase_800342AC_stepped(ctx); return 1;
    case 0x800342ECu: phase_800342EC_stepped(ctx); return 1;
    default: return 0;
    }
}
