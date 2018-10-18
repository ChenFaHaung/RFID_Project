function [output] = plot_phase(obj_p, hand_p, size, mod, fast)
    
    ax2 = subplot(2,1,1);
    axis([0 size+50 -4 4]);
    
    plot(obj_p,'r');
    hold on;
    plot(hand_p, 'bl');
    title('Phase change Static');
    legend({'Object','Hand'},'FontSize', 6,'Location','southeast')
    legend('boxoff');
    xlabel(ax2,'Movement (cm.)', 'Color', [0 0.5 0]);
    ylabel(ax2,'Phase (rad.)', 'Color', [0 0.5 0]);
    
    ax1 = subplot(2,1,2);
    
    h = animatedline();
    h1 = animatedline();
    axis([0 size+50 -4 4]);
    
    title('Phase change Dynamic');
    legend({'Object','Hand'},'FontSize', 6,'Location','southeast')
    legend('boxoff');
    xlabel(ax1,'Movement (cm.)', 'Color', [0 0.5 0]);
    ylabel(ax1,'Phase (rad.)', 'Color', [0 0.5 0]);
    
    for t = 1:size+1
        if(mod == 0)
            addpoints(h,t,obj_p(t));
        end
        addpoints(h1,t,hand_p(t));
        h.Color = 'r';
        h1.Color = 'b';
        drawnow limitrate;
        if(fast == 1)
            pause(5/10000);
        end
    end
end