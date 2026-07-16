// ==========================================
// Function: TSP_GetXY @ 0x00010024
// ==========================================

bool TSP_GetXY(int *param_1,int *param_2,int param_3)

{
  bool bVar1;
  int *piVar2;
  int *piVar3;
  int iVar4;
  int *piVar5;
  int *piVar6;
  int iVar7;
  int local_40 [4];
  int local_30 [5];
  
  iVar4 = ark_adc_mmio_base;
  if (param_3 != 0) {
    piVar6 = local_40;
    iVar7 = (count_17305 % 4) * 4;
    *(uint *)(point_x_17308 + iVar7) = *(uint *)(ark_adc_mmio_base + 0x24) >> 1;
    *(uint *)(point_y_17309 + iVar7) = *(uint *)(iVar4 + 0x28) >> 1;
    count_17305 = count_17305 + 1;
    if (count_17305 < 5) {
      bVar1 = false;
    }
    else {
      iVar4 = 0;
      piVar2 = (int *)point_y_17309;
      piVar3 = (int *)point_x_17308;
      do {
        local_30[iVar4] = *piVar3;
        local_40[iVar4] = *piVar2;
        iVar4 = iVar4 + 1;
        piVar2 = piVar2 + 1;
        piVar3 = piVar3 + 1;
      } while (iVar4 != 4);
      piVar2 = local_30;
      iVar4 = 0;
      do {
        while (iVar4 = iVar4 + 1, piVar3 = piVar6, piVar5 = piVar2, 3 < iVar4) {
          piVar6 = piVar6 + 1;
          piVar2 = piVar2 + 1;
        }
        do {
          iVar7 = *piVar2;
          piVar5 = piVar5 + 1;
          if (*piVar5 < iVar7) {
            *piVar2 = *piVar5;
            *piVar5 = iVar7;
          }
          iVar7 = *piVar6;
          piVar3 = piVar3 + 1;
          if (*piVar3 < iVar7) {
            *piVar6 = *piVar3;
            *piVar3 = iVar7;
          }
        } while (piVar5 != local_30 + 3);
        piVar6 = piVar6 + 1;
        piVar2 = piVar2 + 1;
      } while (iVar4 != 3);
      *param_1 = local_30[1] + local_30[2] + 1 >> 1;
      *param_2 = local_40[1] + local_40[2] + 1 >> 1;
      iVar4 = local_30[2] - local_30[1];
      if (local_30[2] - local_30[1] < 0x51) {
        iVar4 = local_40[2] - local_40[1];
      }
      bVar1 = iVar4 < 0x51;
    }
    return bVar1;
  }
  count_17305 = 0;
  return false;
}




// ==========================================
// Function: SetDBCNT @ 0x0001019c
// ==========================================

void SetDBCNT(undefined4 param_1)

{
  *(undefined4 *)(ark_adc_mmio_base + 0x2c) = param_1;
  return;
}




// ==========================================
// Function: SetDETInter_new @ 0x000101b0
// ==========================================

void SetDETInter_new(undefined4 param_1)

{
  *(undefined4 *)(ark_adc_mmio_base + 0x30) = param_1;
  return;
}




// ==========================================
// Function: ark1680_ts_open @ 0x000101c4
// ==========================================

undefined4 ark1680_ts_open(void)

{
  return 0;
}




// ==========================================
// Function: ark1680_ts_close @ 0x000101cc
// ==========================================

void ark1680_ts_close(void)

{
  return;
}




// ==========================================
// Function: ark1680_ts_interrupt @ 0x000101d0
// ==========================================

/* WARNING: Restarted to delay deadcode elimination for space: ram */

undefined4 ark1680_ts_interrupt(undefined4 param_1,undefined4 *param_2)

{
  int iVar1;
  uint uVar2;
  undefined4 uVar3;
  
  uVar3 = *param_2;
  uVar2 = *(uint *)(ark_adc_mmio_base + 0xc);
  if (lg_ulCurIndex < 0x200) {
    *(uint *)(int_flag + lg_ulCurIndex * 0x44) = uVar2;
    iVar1 = ark_adc_mmio_base;
  }
  else {
    sampleEnd = 1;
    iVar1 = ark_adc_mmio_base;
  }
  for (; ark_adc_mmio_base = iVar1, uVar2 != 0; uVar2 = uVar2 & 0xffff7fff) {
    if ((uVar2 & 1) != 0) {
      *(uint *)(iVar1 + 0xc) = *(uint *)(iVar1 + 0xc) & 0xfffffffe;
      uVar2 = uVar2 & 0xfffffffe;
      printk("%s %d: 0_START: 0x%0x\n","ark1680_ts_interrupt",0x103,*(undefined4 *)(iVar1 + 0x14));
    }
    iVar1 = ark_adc_mmio_base;
    if ((uVar2 & 2) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xfffffffd;
      uVar2 = uVar2 & 0xfffffffd;
      printk("%s %d: 0_STOP : 0x%0x \n","ark1680_ts_interrupt",0x10a,*(undefined4 *)(iVar1 + 0x14));
    }
    iVar1 = ark_adc_mmio_base;
    if ((uVar2 & 4) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xfffffffb;
      uVar2 = uVar2 & 0xfffffffb;
      printk("%s %d: 0_VALUE : 0x%0x \n","ark1680_ts_interrupt",0x111,*(undefined4 *)(iVar1 + 0x14))
      ;
    }
    if ((uVar2 & 8) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xfffffff7;
      uVar2 = uVar2 & 0xfffffff7;
      printk("%s %d: 1_START\n","ark1680_ts_interrupt",0x119);
    }
    if ((uVar2 & 0x10) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffffef;
      uVar2 = uVar2 & 0xffffffef;
      printk("%s %d: 1_STOP\n","ark1680_ts_interrupt",0x120);
    }
    iVar1 = ark_adc_mmio_base;
    if ((uVar2 & 0x20) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffffef;
      uVar2 = uVar2 & 0xffffffdf;
      printk("%s %d: 1_Value =0x%x\n","ark1680_ts_interrupt",0x12a,*(undefined4 *)(iVar1 + 0x18));
    }
    if ((uVar2 & 0x40) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xfffffff7;
      uVar2 = uVar2 & 0xffffffbf;
      printk("%s %d: 2_START_INT\n","ark1680_ts_interrupt",0x131);
    }
    if ((uVar2 & 0x80) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffffef;
      uVar2 = uVar2 & 0xffffff7f;
      printk("%s %d: 2_STOP_INT\n","ark1680_ts_interrupt",0x139);
    }
    iVar1 = ark_adc_mmio_base;
    if ((uVar2 & 0x100) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffffef;
      uVar2 = uVar2 & 0xfffffeff;
      printk("%s %d: 2_VALUE_INT ADCValue = 0x%0x \n","ark1680_ts_interrupt",0x141,
             *(undefined4 *)(iVar1 + 0x1c));
    }
    if ((uVar2 & 0x200) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffffef;
      uVar2 = uVar2 & 0xfffffdff;
      printk("%s %d: AUX3_START_INT\n","ark1680_ts_interrupt",0x148);
    }
    if ((uVar2 & 0x400) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffffef;
      uVar2 = uVar2 & 0xfffffbff;
      printk("%s %d: 3_STOP_INT\n","ark1680_ts_interrupt",0x14f);
    }
    iVar1 = ark_adc_mmio_base;
    if ((uVar2 & 0x800) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffffdf;
      uVar2 = uVar2 & 0xfffff7ff;
      printk("%s %d: 3_VALUE_INT ADCValue = 0x%0x \n","ark1680_ts_interrupt",0x157,
             *(undefined4 *)(iVar1 + 0x20));
    }
    if ((uVar2 & 0x3000) == 0x3000) {
      cnt_17333 = 0;
      TSP_GetXY(&TmpX_17331,&TmpY_17332);
      if (Tspsta_17334 == 0) {
        *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffefff;
        uVar2 = uVar2 & 0xffffefff;
        Tspsta_17334 = 1;
      }
      else {
        *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffdfff;
        uVar2 = uVar2 & 0xffffdfff;
        input_event(uVar3,3,0x18,0);
        input_event(uVar3,1,0x14a,0);
        input_event(uVar3,0,0,0);
        Tspsta_17334 = 0;
      }
    }
    if ((uVar2 & 0x1000) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffefff;
      uVar2 = uVar2 & 0xffffefff;
      cnt_17333 = 0;
      TSP_GetXY(&TmpX_17331,&TmpY_17332);
      if (Tspsta_17334 == 0) {
        Tspsta_17334 = 1;
      }
      else {
        input_event(uVar3,3,0x18,0);
        input_event(uVar3,1,0x14a,0);
        input_event(uVar3,0,0,0);
        Tspsta_17334 = 0;
      }
    }
    if ((uVar2 & 0x2000) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffdfff;
      uVar2 = uVar2 & 0xffffdfff;
      cnt_17333 = 0;
      TSP_GetXY(&TmpX_17331,&TmpY_17332,0);
      Tspsta_17334 = 0;
      input_event(uVar3,3,0x18,0);
      input_event(uVar3,1,0x14a,0);
      input_event(uVar3,0,0,0);
    }
    if ((uVar2 & 0x4000) != 0) {
      *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffffbfff;
      uVar2 = uVar2 & 0xffffbfff;
      cnt_17333 = cnt_17333 + 1;
      if (cnt_17333 < 1) {
        TSP_GetXY(&TmpX_17331,&TmpY_17332,0);
      }
      else {
        iVar1 = TSP_GetXY(&TmpX_17331,&TmpY_17332,1);
        if (iVar1 != 0) {
          PrevX_17329 = TmpX_17331;
          PrevY_17330 = TmpY_17332;
          input_event(uVar3,3,0);
          input_event(uVar3,3,1,PrevY_17330);
          input_event(uVar3,3,0x18,0xfff);
          input_event(uVar3,1,0x14a,1);
          input_event(uVar3,0,0,0);
        }
      }
      Tspsta_17334 = 2;
    }
    if ((uVar2 & 0x8000) == 0) break;
    *(uint *)(ark_adc_mmio_base + 0xc) = *(uint *)(ark_adc_mmio_base + 0xc) & 0xffff7fff;
    iVar1 = ark_adc_mmio_base;
  }
  if (sampleEnd == 0) {
    lg_ulCurIndex = lg_ulCurIndex + 1;
  }
  return 1;
}




// ==========================================
// Function: DivADCCLK @ 0x00010824
// ==========================================

void DivADCCLK(uint param_1)

{
  int iVar1;
  
  iVar1 = ark_sys_mmio_base;
  *(uint *)(ark_sys_mmio_base + 100) = *(uint *)(ark_sys_mmio_base + 100) & 0xffff0001;
  *(uint *)(iVar1 + 100) = (param_1 & 0x7fff) << 1 | *(uint *)(iVar1 + 100);
  return;
}




// ==========================================
// Function: Enable_ADC_Channel @ 0x00010858
// ==========================================

void Enable_ADC_Channel(undefined4 param_1)

{
  uint *puVar1;
  
  puVar1 = ark_adc_mmio_base;
  switch(param_1) {
  case 1:
    *ark_adc_mmio_base = *ark_adc_mmio_base | 2;
    puVar1[2] = puVar1[2] & 0xffff7fff;
    return;
  case 2:
    *ark_adc_mmio_base = *ark_adc_mmio_base | 4;
    puVar1[3] = 0;
    puVar1[2] = puVar1[2] & 0xffff8fff;
    return;
  case 3:
    *ark_adc_mmio_base = *ark_adc_mmio_base | 8;
    puVar1[3] = 0;
    puVar1[2] = puVar1[2] & 0xfffffff8;
    return;
  case 4:
    *ark_adc_mmio_base = *ark_adc_mmio_base | 0x10;
    puVar1[2] = puVar1[2] & 0xffffffc7;
    return;
  case 5:
    *ark_adc_mmio_base = *ark_adc_mmio_base | 0x20;
    puVar1[2] = puVar1[2] & 0xfffffe3f;
    return;
  case 6:
    *ark_adc_mmio_base = *ark_adc_mmio_base | 0x40;
    puVar1[2] = puVar1[2] & 0xfffff1ff;
  }
  return;
}




// ==========================================
// Function: ark1680_setup_tsc @ 0x00010980
// ==========================================

void ark1680_setup_tsc(void)

{
  uint *puVar1;
  int iVar2;
  
  DivADCCLK(0xef);
  iVar2 = ark_sys_mmio_base;
  *(uint *)(ark_sys_mmio_base + 0x48) = *(uint *)(ark_sys_mmio_base + 0x48) | 8;
  *(uint *)(iVar2 + 0x50) = *(uint *)(iVar2 + 0x50) | 0x800;
  *(uint *)(iVar2 + 0x140) = *(uint *)(iVar2 + 0x140) & 0xffbfffff;
  puVar1 = ark_adc_mmio_base;
  *ark_adc_mmio_base = *ark_adc_mmio_base | 1;
  *puVar1 = *puVar1 & 0xffffff81;
  puVar1[2] = 7;
  puVar1[3] = 0;
  SetDBCNT(40000);
  SetDETInter_new(0x82);
  *(uint *)(iVar2 + 0x144) = *(uint *)(iVar2 + 0x144) & 0xffffbfff;
  *puVar1 = *puVar1 | 0x100;
  *puVar1 = *puVar1 | 0x200;
  *puVar1 = *puVar1 | 0x400;
  *puVar1 = *puVar1 | 0x800;
  Enable_ADC_Channel(2);
  return;
}




// ==========================================
// Function: ark1680_ts_remove @ 0x00010a48
// ==========================================

undefined4 ark1680_ts_remove(int param_1)

{
  undefined4 *puVar1;
  int *piVar2;
  
  puVar1 = (undefined4 *)dev_get_drvdata(param_1 + 8);
  *(byte *)(param_1 + 0x58) = *(byte *)(param_1 + 0x58) & 0xfe;
  *(byte *)(param_1 + 0x59) = *(byte *)(param_1 + 0x59) & 0xfe;
  free_irq(puVar1[2],puVar1);
  input_unregister_device(*puVar1);
  __arm_iounmap(puVar1[1]);
  piVar2 = (int *)platform_get_resource(param_1,0x200,0);
  __release_region(&iomem_resource,*piVar2,(piVar2[1] + 1) - *piVar2);
  kfree(puVar1);
  return 0;
}




// ==========================================
// Function: ark1680_ts_probe @ 0x00010ad0
// ==========================================

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

int ark1680_ts_probe(undefined4 *param_1)

{
  int *piVar1;
  int iVar2;
  undefined4 *puVar3;
  undefined4 *puVar4;
  int iVar5;
  undefined4 *puVar6;
  
  piVar1 = (int *)platform_get_resource(param_1,0x200,0);
  if (piVar1 == (int *)0x0) {
    dev_err(param_1 + 2,"Can\'t get memory resource\n");
    iVar2 = -2;
  }
  else {
    iVar2 = platform_get_irq(param_1,0);
    if (iVar2 < 0) {
      dev_err(param_1 + 2,"Can\'t get interrupt resource\n");
    }
    else {
      if (_DAT_0001a078 == 0) {
        puVar4 = (undefined4 *)0x10;
      }
      else {
        puVar4 = (undefined4 *)kmem_cache_alloc(_DAT_0001a078,0x80d0);
      }
      puVar3 = (undefined4 *)input_allocate_device();
      if (puVar4 == (undefined4 *)0x0 || puVar3 == (undefined4 *)0x0) {
        dev_err(param_1 + 2,"failed allocating memory\n");
        iVar2 = -0xc;
      }
      else {
        *puVar4 = puVar3;
        puVar4[2] = iVar2;
        iVar5 = (piVar1[1] + 1) - *piVar1;
        iVar2 = __request_region(&iomem_resource,*piVar1,iVar5,*param_1,0);
        if (iVar2 == 0) {
          dev_err(param_1 + 2,"TSC registers are not free\n");
          iVar2 = -0x10;
        }
        else {
          iVar2 = __arm_ioremap(*piVar1,iVar5,0);
          puVar4[1] = iVar2;
          if (iVar2 == 0) {
            dev_err(param_1 + 2,"Can\'t map memory\n");
            iVar2 = -0xc;
          }
          else {
            ark_adc_mmio_base = iVar2;
            ark_sys_mmio_base = __arm_ioremap(0xe4900000,0x200,0);
            if (ark_sys_mmio_base == 0) {
              printk("<3>%s %d: failed to ioremap registers, start=%08x end=%08x\n",DAT_00010e30,
                     0x28a,0xe4900000,0xe4900200);
              iVar2 = -0x13;
            }
            else {
              ark1680_setup_tsc();
              *puVar3 = "ark1680-ts";
              puVar3[1] = "ark1680/input0";
              *(undefined2 *)(puVar3 + 3) = 0x19;
              *(undefined2 *)((int)puVar3 + 0xe) = 1;
              *(undefined2 *)(puVar3 + 4) = 2;
              *(undefined2 *)((int)puVar3 + 0x12) = 0x100;
              puVar6 = param_1 + 2;
              puVar3[0x66] = puVar6;
              puVar3[0x5b] = ark1680_ts_open;
              puVar3[0x5c] = ark1680_ts_close;
              puVar3[6] = 10;
              puVar3[0x11] = 0x400;
              input_set_abs_params(puVar3,0,0,800,0,0);
              input_set_abs_params(puVar3,1,0,0x1e0,0,0);
              input_set_abs_params(puVar3,0x18,0,0xfff,0,0);
              dev_set_drvdata(puVar3 + 0x66,puVar4);
              iVar2 = request_threaded_irq(puVar4[2],ark1680_ts_interrupt,0,0,*param_1,puVar4);
              if (iVar2 == 0) {
                printk("%s %d: request_irq:%d success \n",DAT_00010e30,0x2ab,puVar4[2]);
                iVar2 = input_register_device(puVar3);
                if (iVar2 == 0) {
                  dev_set_drvdata(puVar6,puVar4);
                  *(byte *)(param_1 + 0x16) = *(byte *)(param_1 + 0x16) | 1;
                  *(byte *)((int)param_1 + 0x59) = *(byte *)((int)param_1 + 0x59) | 1;
                  return 0;
                }
                dev_err(puVar6,"failed registering input device\n");
                free_irq(puVar4[2],puVar4);
              }
              else {
                dev_err(puVar6,"failed requesting interrupt\n");
              }
              __arm_iounmap(puVar4[1]);
            }
          }
          __release_region(&iomem_resource,*piVar1,iVar5);
        }
      }
      input_free_device(puVar3);
      kfree(puVar4);
    }
  }
  return iVar2;
}




// ==========================================
// Function: init_module @ 0x00010e34
// ==========================================

void init_module(void)

{
  platform_driver_register(&ark1680_ts_driver);
  return;
}




// ==========================================
// Function: cleanup_module @ 0x00010e48
// ==========================================

void cleanup_module(void)

{
  platform_driver_unregister(&ark1680_ts_driver);
  return;
}




// ==========================================
// Function: input_allocate_device @ 0x0001a000
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void input_allocate_device(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: free_irq @ 0x0001a004
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void free_irq(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: dev_get_drvdata @ 0x0001a008
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void dev_get_drvdata(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: platform_driver_unregister @ 0x0001a00c
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void platform_driver_unregister(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: platform_get_irq @ 0x0001a010
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void platform_get_irq(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: __arm_ioremap @ 0x0001a018
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void __arm_ioremap(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: input_unregister_device @ 0x0001a01c
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void input_unregister_device(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: kfree @ 0x0001a020
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void kfree(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: __release_region @ 0x0001a024
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void __release_region(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: input_free_device @ 0x0001a028
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void input_free_device(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: __arm_iounmap @ 0x0001a02c
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void __arm_iounmap(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: input_register_device @ 0x0001a030
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void input_register_device(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: kmem_cache_alloc @ 0x0001a034
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void kmem_cache_alloc(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: platform_driver_register @ 0x0001a038
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void platform_driver_register(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: request_threaded_irq @ 0x0001a03c
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void request_threaded_irq(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: platform_get_resource @ 0x0001a040
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void platform_get_resource(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: printk @ 0x0001a044
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void printk(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: dev_err @ 0x0001a048
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void dev_err(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: input_event @ 0x0001a04c
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void input_event(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: input_set_abs_params @ 0x0001a050
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void input_set_abs_params(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: dev_set_drvdata @ 0x0001a054
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void dev_set_drvdata(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




// ==========================================
// Function: __request_region @ 0x0001a064
// ==========================================

/* WARNING: Control flow encountered bad instruction data */

void __request_region(void)

{
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}




