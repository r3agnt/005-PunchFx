#pragma once
// The real reverb for the reverb bus: DaisySP's ReverbSc (LGPL part of DaisySP; header
// DaisySP-LGPL/Source/Effects/reverbsc.h, needs USE_DAISYSP_LGPL=1 in the Makefile).
// ReverbSc holds about 395 KB of audio memory inside the object, so the object itself is created in
// main.cpp in the external SDRAM (DSY_SDRAM_BSS) and only attached here.
#include "Effects/reverbsc.h"
#include "../engine/fx/fx_reverb_bus.h"

class ScReverbCore : public fx::ReverbCore
{
  public:
    void Attach(daisysp::ReverbSc *sc) { sc_ = sc; }
    void Init(float sample_rate) override { sc_->Init(sample_rate); } // also clears its delay lines
    void SetFeedback(float fb) override { sc_->SetFeedback(fb); }
    void SetToneHz(float hz) override { sc_->SetLpFreq(hz); }
    void Process(float in_l, float in_r, float *out_l, float *out_r) override
    {
        sc_->Process(in_l, in_r, out_l, out_r);
    }

  private:
    daisysp::ReverbSc *sc_ = nullptr;
};
