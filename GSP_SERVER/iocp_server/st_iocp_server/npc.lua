my_id = -1
--move_stack = 0

my_level = 1
my_max_hp = 100
my_current_exp = 0
my_power = 10
my_state = 0
my_range = 1

function on_create()
    API_send_mess(0, my_id, "HELLO")
end

function set_object_id(x)
    my_id = x
end

function set_status(level, max_hp, current_exp, power, state)
    my_level = level
    my_max_hp = max_hp
    my_current_exp = current_exp
    my_power = power
    my_state = state
end

function set_level(level)
    my_level = level
end

function set_max_hp(max_hp)
    my_max_hp = max_hp
end

function set_current_exp(current_exp)
    my_current_exp = current_exp
end

function set_power(power)
    my_power = power
end

function player_is_near(p_id)
    p_x = API_get_x(p_id)
    p_y = API_get_y(p_id)
    m_x = API_get_x(my_id)
    m_y = API_get_y(my_id)

    if (p_x == m_x) then
        if (p_y == m_y) then
            API_send_mess(p_id, my_id, "HELLO")
            return true
        end
    end
end

function on_update()
    --if (move_stack > 0) then
    --	move_stack = move_stack - 1
    --	return
    --end
    --
    --if (my_state == 0) then
    --	API_move(my_id, 0, 1)
    --	move_stack = 10
    --end
end

function get_power()
    return my_power
end
