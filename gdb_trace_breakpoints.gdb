target remote localhost:3333
mon reset halt
break lcd_driver_draw_frame
break on_lcd_trans_done
info breakpoints
c
print s_dma_done_sem
c
print s_dma_done_sem
detach
quit
