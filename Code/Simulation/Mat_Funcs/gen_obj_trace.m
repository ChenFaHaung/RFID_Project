function [out1, out2] = gen_obj_trace(obj_num, xi, yi, pace, dis, dir, slope, flag, noise, inter, mod)
    
    PLOT_SIZE = dis / pace;
    %out1(1,:) = xi(1);
    %out2(1,:) = yi(1);
    for i = 1:obj_num
        if(mod == 0)
            if(flag == 1) % vertical line
                out1(i,:) = xi(i) + ones(1,(PLOT_SIZE)).*inter;

                yi_1_r = (yi(i)+pace * dir : pace * dir : (yi(i) + dis * dir));
                out2(i,:) = yi_1_r;
            else
                y_shift_obj = yi(i) - slope .* xi(i);
                out1_tmp = (xi(i)+pace : pace : (xi(i) + dis));
                out2(i,:) = (slope + inter) .* out1_tmp + y_shift_obj + noise;
                out1(i,:) = out1_tmp;
            end
            
        elseif(mod == 1) % don't move
            out1(i,:) = (xi(i) : 0 : (xi(i) + dis));
            out2 = out1;
        end
    end
end