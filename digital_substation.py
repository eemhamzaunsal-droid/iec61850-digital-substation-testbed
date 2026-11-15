import pandapower as pp
import pandapower.networks as pn
import matplotlib.pyplot as plt

def create_digital_substation_net():
    """
    IEEE 14-Bus Test Sistemini yükler ve Fider 1 için gerekli CT ve CB'leri ekler.
    """
    # 1. IEEE 14 Baralı standart sistemi yükle
    net = pn.case14()
    
    # 2. Hat 4'ü "Fider 1" olarak seçeceğiz (Bara 1'den Bara 4'e giden hat)
    target_line_idx = 4
    source_bus = net.line.at[target_line_idx, 'from_bus']
    
    # 3. CB1 Kesicisini Ekle
    pp.create_switch(net, bus=source_bus, element=target_line_idx, et="l", closed=True, type="CB", name="CB1")
    
    # 4. CT1 Ölçüm Noktasını Ekle (Akım Trafosu)
    pp.create_measurement(net, "i", "line", 0.0, 0.01, element=target_line_idx, side="from", name="CT1")
    
    # 5. Hat İsimlendirmesini Güncelle (Streamer.py için kolaylık)
    net.line.at[target_line_idx, "name"] = "Fider 1 Hatti"
    
    # 6. Yük İsimlendirmesi (Bara 4'e bağlı olan yükü "Yuk 1" olarak isimlendir)
    # Bu, klavye simülasyonlarında arıza üretmek için kullanılacaktır.
    target_bus = net.line.at[target_line_idx, 'to_bus']
    load_idx = net.load[net.load.bus == target_bus].index
    if len(load_idx) > 0:
        net.load.at[load_idx[0], "name"] = "Yuk 1"
        
    return net

def draw_custom_diagram(net):
    """
    Pandapower'ın dahili çizici fonksiyonunu kullanarak basit bir topoloji çizimi yapar.
    """
    try:
        import pandapower.plotting as plot
        plot.simple_plot(net, show_plot=False)
    except Exception as e:
        print(f"Pandapower plotting modülü kullanılamıyor: {e}")
