target remote localhost:3333
mon reset halt
break on_lcd_trans_done
break lcd_driver_draw_frame
break sd_reader_task
info breakpoints
print s_dma_done_sem
c
print s_dma_done_sem
info symbol s_dma_done_sem
c
print s_dma_done_sem
bt
quit
