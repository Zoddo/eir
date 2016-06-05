#include "eir.h"

using namespace eir;
using namespace std::placeholders;

#include <list>
#include "match.h"

#include <paludis/util/join.hh>
#include <paludis/util/tokeniser.hh>

#include "times.h"

#include <algorithm>

#include "help.h"

namespace
{
    const std::string default_time_fmt("%F %T");
    const std::string default_expiry("1d");

    std::string format_time(Bot *b, time_t t)
    {
        if (t == 0)
            return "never";

        char datebuf[128];
        tm time;
        localtime_r(&t, &time);
        strftime(datebuf,
                 sizeof(datebuf),
                 b->get_setting_with_default("op_time_format", default_time_fmt).c_str(),
                 &time);
        return std::string(datebuf);
    }

    time_t get_default_expiry(Bot *b)
    {
        std::string time = b->get_setting_with_default("default_op_expiry", default_expiry);
        return parse_time(time);
    }

    time_t get_reop_expiry(Bot *b)
    {
        std::string time = b->get_setting_with_default("reop_expiry", default_expiry);
        return parse_time(time);
    }

    const char *help_opbot =
        "The opbot module exists to manage a moderated channel. It maintains a list of hostmask entries that "
        "will not be opped (the \002DNO\002 list), and ops, on request, all unopped users in the channel that "
        "do not match any of those entries.\n"
        "If opbot_enable_reopping is set users who leave the channel while opped will be reopped automatically "
        "should they return before the time specified in reop_expiry has elapsed.\n "
        "Available opbot commands are: op check match add remove edit.\n "
        "Relevant settings are \037opbot_channel\037, \037opbot_admin_channel\037, \037default_op_expiry\037, "
        "\037opbot_enable_reopping\037, \037reop_expiry\037 and \037op_time_format\037.";
    const char *help_op =
        "\002op\002. Ops all users who are in \037opbot_channel\037 and do not match a DNO list entry.";
    const char *help_check =
        "\002check\002. Displays a list of users in \037opbot_channel\037 who would be opped by the \002op\002 "
        "command, and a list of those unopped users who would not be opped.";
    const char *help_match =
        "\002match <mask>|<nick>\002. Displays the DNO list entries that match the given argument. If the nickname "
        "of a currently visible user is given, entries matching that user are shown. Otherwise, it is interpreted as "
        "a hostmask.";
    const char *help_add =
        "\002add <mask> [time] <reason>\002. Adds a DNO entry <mask>, expiring in <time>, with comment <reason>.\n"
        "If \037time\037 is not specified, the default_op_expiry setting is used. \037time\037 may "
        "be zero, in which case the entry will not expire. \037reason\037 may be empty.";
    const char *help_remove =
        "\002remove <mask>\002. Removes all entries from the DNO list that match the given mask.\n"
        "Note that 'remove *' will clear the list.";
    const char *help_edit =
        "\002edit <mask> [time] [reason]\002. Edits the expiry time and/or comment of an existing DNO entry. If "
        "\037time\037 is given but \037reason\037 is not, then only the expiry will be changed. Similarly if "
        "\037reason\037 is given but \037time\037 is not, then the expiry will be left unchanged.";


    Value opentry(std::string bot, std::string mask, std::string setter, std::string reason,
                     time_t set, time_t expires)
    {
        Value v(Value::kvarray);
        v["bot"] = bot;
        v["mask"] = mask;
        v["setter"] = setter;
        v["reason"] = reason;
        v["set"] = set;
        v["expires"] = expires;
        return v;
    }

    Value lostopentry(std::string bot, std::string mask, time_t expires)
    {
        Value v(Value::kvarray);
        v["bot"] = bot;
        v["mask"] = mask;
        v["expires"] = expires;
        return v;
    }

    struct Removed
    {
        bool operator() (const Value& v)
        {
            return v.Type() == Value::kvarray && v.KV().find("removed") != v.KV().end();
        }
    };

    void do_removals(Value& list)
    {
        Value newlist(Value::array);
        std::remove_copy_if(list.begin(), list.end(), std::back_inserter(newlist.Array()), Removed());
        std::swap(list, newlist);
    }
}

struct opbot : CommandHandlerBase<opbot>, Module
{
    Value &dno, &old, &lostops;

    void do_add(const Message *m)
    {
        if (m->args.empty())
        {
            m->source.error("Need at least one argument");
            return;
        }

        time_t expires;

        std::vector<std::string>::const_iterator it = m->args.begin();
        std::string mask = *it++;

        if ((*it)[0] == '~')
            expires = parse_time(*it++);
        else
            expires = get_default_expiry(m->bot);

        if (expires != 0)
            expires += time(NULL);

        std::string reason = paludis::join(it, m->args.end(), " ");

        if (reason.empty())
        {
            m->source.error("You need to specify a reason.");
            return;
        }

        // If this looks like a plain nick instead of a mask, treat it as a nickname
        // mask.
        if (mask.find_first_of("!@*") == std::string::npos)
            mask += "!*@*";

        for (ValueArray::iterator it = dno.begin(); it != dno.end(); ++it)
        {
            if (mask_match((*it)["mask"], mask))
            {
                m->source.reply("Mask already matched by " + (*it)["mask"]);
                return;
            }
        }

        dno.push_back(opentry(m->bot->name(), mask, m->source.name, reason, time(NULL), expires));
        m->source.reply("Added " + mask);

        Logger::get_instance()->Log(m->bot, m->source.client, Logger::Command, "ADD " + mask);
    }

    void do_change(const Message *m)
    {
        if (m->args.empty())
        {
            m->source.error("Need at least one argument");
            return;
        }

        time_t expires = 0;

        std::vector<std::string>::const_iterator it = m->args.begin();
        std::string mask = *it++;

        if ((*it)[0] == '~')
            expires = parse_time(*it++);
        if (expires > 0)
            expires += time(NULL);

        std::string reason = paludis::join(it, m->args.end(), " ");

        // If this looks like a plain nick instead of a mask, treat it as a nickname
        // mask.
        if (mask.find_first_of("!@*") == std::string::npos)
            mask += "!*@*";

        bool found = false;

        for (ValueArray::iterator it = dno.begin(); it != dno.end(); ++it)
        {
            if (mask_match(mask, (*it)["mask"]))
            {
                if (expires)
                    (*it)["expires"] = expires;
                if (!reason.empty())
                    (*it)["reason"] = reason;
                found = true;
                m->source.reply("Updated " + (*it)["mask"]);
            }
        }
        if (!found)
            m->source.reply("No entry matches " + mask);

        Logger::get_instance()->Log(m->bot, m->source.client, Logger::Command, "CHANGE " + mask);
    }

    void do_remove(const Message *m)
    {
        if (m->args.empty())
        {
            m->source.error("Need at least one argument");
            return;
        }

        std::string mask = m->args[0];

        if (mask.find_first_of("!@*") == std::string::npos)
            mask += "!*@*";

        for (ValueArray::iterator it = dno.begin(); it != dno.end(); ++it)
        {
            if (mask_match(mask, (*it)["mask"]))
            {
                Bot *bot = BotManager::get_instance()->find((*it)["bot"]);
                m->source.reply("Removing " + (*it)["mask"] + " (" + (*it)["reason"] + ") " +
                        "(added by " + (*it)["setter"] + " on " + format_time(bot, (*it)["set"].Int()) + ")");

                old.push_back(*it);
                (*it)["removed"] = 1;
            }
        }

        do_removals(dno);

        Logger::get_instance()->Log(m->bot, m->source.client, Logger::Command, "REMOVE " + mask);
    }

    void do_list(const Message *m)
    {
        for (ValueArray::iterator it = dno.begin(); it != dno.end(); ++it)
        {
            Bot *bot = BotManager::get_instance()->find((*it)["bot"]);
            m->source.reply((*it)["mask"] + " (" + (*it)["reason"] + ") (added by " +
                    (*it)["setter"] + " on " + format_time(bot, (*it)["set"].Int()) +
                    ", expires " + format_time(bot, (*it)["expires"].Int()) + ")");
        }

        m->source.reply("*** End of DNO list");
    }

    void build_op_lists(Channel::ptr channel,
                           std::list<std::string> *toop,
                           std::list<std::string> *tonotop)
    {
        for (Channel::MemberIterator it = channel->begin_members(); it != channel->end_members(); ++it)
        {
            if ((*it)->has_mode('o'))
                continue;

            bool matched = false;
            for (ValueArray::iterator i2 = dno.begin(); i2 != dno.end(); ++i2)
            {
                if (match((*i2)["mask"], (*it)->client->nuh()))
                {
                    matched = true;
                    break;
                }
            }

            if (matched)
                tonotop->push_back((*it)->client->nick());
            else
                toop->push_back((*it)->client->nick());
        }
    }

    void do_check(const Message *m)
    {
        std::string channelname = m->bot->get_setting("opbot_channel");
        if (channelname.empty())
        {
            m->source.error("opbot_channel not defined.");
            return;
        }

        Channel::ptr channel = m->bot->find_channel(channelname);
        if (!channel)
        {
            m->source.error("Couldn't find channel " + channelname);
            return;
        }

        std::list<std::string> toop, tonotop;

        build_op_lists(channel, &toop, &tonotop);

		if (!toop.empty())
		{
			m->source.reply("Needing op: " + paludis::join(toop.begin(), toop.end(), " "));
		}
		else
		{
			m->source.reply("Everybody is opped.");
		}

        if (!tonotop.empty())
		{
			m->source.reply("Not opping: " + paludis::join(tonotop.begin(), tonotop.end(), " "));
		}
    }

    void do_op(const Message *m)
    {
        std::string channelname = m->bot->get_setting("opbot_channel");
        if (channelname.empty())
        {
            m->source.error("opbot_channel not defined.");
            return;
        }

        Channel::ptr channel = m->bot->find_channel(channelname);
        if (!channel)
        {
            m->source.error("Couldn't find channel " + channelname);
            return;
        }

        std::list<std::string> toop, tonotop;

        build_op_lists(channel, &toop, &tonotop);

        while (!toop.empty())
        {
            std::list<std::string> thisoprun;

            int i;

            for (i = 0; i < m->bot->supported()->max_modes() && !toop.empty(); ++i)
            {
                thisoprun.push_back(*toop.begin());
                toop.pop_front();
            }
            std::string opcommand = "MODE " + channelname + " " "+" + std::string(i, 'o') + " " +
                                       paludis::join(thisoprun.begin(), thisoprun.end(), " ");
            m->bot->send(opcommand);
        }

        Logger::get_instance()->Log(m->bot, m->source.client, Logger::Command, "OP");
    }

    void do_match(const Message *m)
    {
        if (m->args.empty())
        {
            m->source.error("Need one argument");
            return;
        }

        std::string mask = m->args[0];
        if (mask.find_first_of("!@*") == std::string::npos)
        {
            Client::ptr c = m->bot->find_client(mask);
            if (!c)
                mask += "!*@*";
            else
                mask = c->nuh();
        }

        for (ValueArray::iterator it = dno.begin(); it != dno.end(); ++it)
            if (mask_match((*it)["mask"], mask))
            {
                Bot *bot = BotManager::get_instance()->find((*it)["bot"]);
                m->source.reply((*it)["mask"] + " (" + (*it)["reason"] + ") (added by " +
                    (*it)["setter"] + " on " + format_time(bot, (*it)["set"].Int()) +
                    ", expires " + format_time(bot, (*it)["expires"].Int()) + ")");
            }

        m->source.reply("*** End of DNO matches for " + mask);
    }

    void check_expiry()
    {
        time_t currenttime = time(NULL);

        for (ValueArray::iterator it = dno.begin(); it != dno.end(); ++it)
        {
            if ((*it)["expires"].Int() != 0 && (*it)["expires"].Int() < currenttime)
            {
                Bot *bot = BotManager::get_instance()->find((*it)["bot"]);
                std::string adminchan;
                if (bot)
                    adminchan = bot->get_setting("opbot_admin_channel");

                if (bot && !adminchan.empty())
                    bot->send("NOTICE " + adminchan + " :Removing expired entry " +
                            (*it)["mask"] + " added by " + (*it)["setter"] + " on " +
                            format_time(bot, (*it)["set"].Int()));

                old.push_back(*it);
                (*it)["removed"] = 1;
            }
        }
        for (ValueArray::iterator it = lostops.begin(); it != lostops.end(); ++it)
        {
            if ((*it)["expires"].Int() != 0 && (*it)["expires"].Int() < currenttime)
            {
                (*it)["removed"] = 1;
            }
        }

        do_removals(dno);
        do_removals(lostops);
    }

    void load_list(Value & v, std::string name)
    {
        try
        {
            v = StorageManager::get_instance()->Load(name);
            if (v.Type() != Value::array)
                throw "wrong type";
        }
        catch (StorageError &)
        {
            v = Value(Value::array);
        }
        catch (IOError &)
        {
            v = Value(Value::array);
        }
        catch (const char *)
        {
            Logger::get_instance()->Log(NULL, NULL, Logger::Warning,
                    "Loaded op list " + name + "has wrong type; ignoring");
            v = Value(Value::array);
        }
    }

    void load_lists()
    {
        load_list(dno, "donotop");
        load_list(old, "expireddonotop");
        load_list(lostops, "lostops");
    }

    std::string build_reop_mask (Client::ptr c)
    {
    /* This is freenode specific, so may need to be changed if running on a network
       with different vhost policies

       in English:

       Uncloaked users are matched on their full nick!user@host mask
       Gateway cloaked users are matched on nick!user@gateway/type/name, minus the x-NNNNNNNNNNNNNNNN session ID
       Authenticated gateway cloaked users (currently only tor-sasl), and regular cloaked users are matched on *!*@cloak

    */

        if (c->host().find("/") == std::string::npos)
        {
            // normal user, return full nuh
            return c->nuh();
        } else if (c->host().find("gateway/tor-sasl/") == 0) {
            // tor-sasl user, return *!*@cloak
            return "*!*@" + c->host();
        } else if (c->host().find("gateway/") == 0 ||
                   c->host().find("conference/") == 0 ||
                   c->host().find("nat/") == 0 )
        {
            // gateway user
            std::string suffix=c->nuh().substr(c->nuh().find_last_of("/"));
            if (suffix=="/session" || suffix.substr(0,3) == "/x-" || suffix.substr(0,4)== "/ip.")
            {
                // strip session ID
                return c->nuh().substr(0,c->nuh().find_last_of("/")) + "/*";
            } else {
                // got an unrecognised suffix - don't return a reop mask
                return "";
            }
        } else {
            // cloaked user, return *!*@cloak
            return "*!*@" + c->host();
        }
    }



    void irc_join(const Message *m)
    {
        std::string reopping = m->bot->get_setting_with_default("opbot_enable_reopping", "");
        std::string channelname = m->bot->get_setting("opbot_channel");

        if (reopping.empty())
            return;
        if (m->source.name == m->bot->nick())
            return;
        if (m->source.destination != channelname)
            return;

        for (ValueArray::iterator it = lostops.begin(); it != lostops.end(); ++it)
        {
            if (mask_match((*it)["mask"], m->source.raw))
            {
                Client::ptr c = m->bot->find_client(m->source.name);
                if (c)
                {
                    std::weak_ptr<Client> w(c);
                    Logger::get_instance()->Log(m->bot, NULL, Logger::Debug, "*** Matched lost op for " + m->source.raw + "(" + (*it)["mask"] + ")");
                    Logger::get_instance()->Log(m->bot, NULL, Logger::Debug, "*** Queueing reop for " + m->source.name);
                    add_event(time(NULL)+5, std::bind(reop, m->bot, w, channelname));
                    (*it)["removed"]=1;
                }
            }
        }
        do_removals(lostops);
    }

    void irc_nick(const Message *m)
    {
        std::string reopping = m->bot->get_setting_with_default("opbot_enable_reopping", "");
        std::string channelname = m->bot->get_setting("opbot_channel");

        if (reopping.empty())
            return;
        if (!m->source.client)
            return;
        if (m->source.name == m->bot->nick())
            return;

        for (ValueArray::iterator it = lostops.begin(); it != lostops.end(); ++it)
        {
            if (mask_match((*it)["mask"], m->source.client->nuh()))
            {
                std::weak_ptr<Client> w(m->source.client);
                Logger::get_instance()->Log(m->bot, NULL, Logger::Debug, "*** Matched lost op for " + m->source.raw + "(" + (*it)["mask"] + ")");
                Logger::get_instance()->Log(m->bot, NULL, Logger::Debug, "*** Queueing reop for " + m->source.destination );
                add_event(time(NULL)+5, std::bind(reop, m->bot, w, channelname));
                (*it)["removed"]=1;
            }
        }
        do_removals(lostops);
    }

    void irc_depart (const Message *m)
    {
        std::string reopping = m->bot->get_setting_with_default("opbot_enable_reopping", "");
        std::string channelname = m->bot->get_setting("opbot_channel");

        if (reopping.empty())
            return;
        if (!m->source.client)
            return;
        if (m->source.name == m->bot->nick())
            return;

        Channel::ptr channel = m->bot->find_channel(channelname);
        if (!channel)
            return;

        Membership::ptr mem = m->source.client->find_membership(channelname);
        if (mem && mem->has_mode('o')) {
            if (m->command == "PART" && m->source.destination == channelname )
            {
                if (m->args.size() >= 1 && m->args[0].substr(0,9) == "requested")
                {
                    // user was ejected from the channel with REMOVE
                    Logger::get_instance()->Log(m->bot, NULL, Logger::Debug, "*** " + m->source.client->nick()  + "was removed from channel - will not reop");
                    return;
                }
            } else if (m->command == "QUIT") {
                if (!m->source.destination.empty())
                {
                    std::string q = m->source.destination.substr(0,m->source.destination.find(" "));
                    if (q == "Killed" ||  q == "K-Lined" ||  q == "Changing" ||  q == "*.net")
                    {
                        // Abnormal quit - ignore
                        Logger::get_instance()->Log(m->bot, NULL, Logger::Debug, "*** " + m->source.client->nick()  + "left network abnormally - will not reop");
                        return;
                    }
                }
            } else {
                // Wrong channel, or something odd happened
                return;
            }
            // User left the channel or network normally while opped - put them on the lost ops list
            std::string mask = m->source.raw;
            mask=build_reop_mask(m->source.client);
            if (!mask.empty())
            {
                // check we don't already have this mask
                for (ValueArray::iterator it = lostops.begin(); it != lostops.end(); ++it)
                {
                    if ((*it)["mask"] == mask)
                    {
                        Logger::get_instance()->Log(m->bot, NULL, Logger::Debug, "*** " + mask + " is already on lostops list, skipping");
                        return;
                    }
                }
                lostops.push_back(lostopentry(m->bot->name(), mask, get_reop_expiry(m->bot)+time(NULL)));
                Logger::get_instance()->Log(m->bot, NULL, Logger::Debug, "*** " + m->source.client->nick() + "(" + mask + ")" + " left " + channelname + " with op");
            }
        }
    }

    static void reop(Bot *bot, std::weak_ptr<Client> w, std::string channel)
    {
        Client::ptr c = w.lock();
        if (!c)
        {
            return;
        }

        Membership::ptr mem=c->find_membership(channel);
        if (!mem)
        {
            return;
        }

        if (mem->has_mode('o'))
        {
            Logger::get_instance()->Log(bot, NULL, Logger::Debug, "**** " + c->nick() + " is alreadly opped on " + channel +", skipping");
        } else {
            Logger::get_instance()->Log(bot, NULL, Logger::Debug, "*** reopping " + c->nick() + " on "+ channel);
            Logger::get_instance()->Log(bot, NULL, Logger::Admin, "*** reopping " + c->nick() + " on "+ channel);
            bot->send("MODE " + channel + " +o " + c->nick());
        }
    }

    CommandHolder add, remove, list, info, check, op, clear, change, match_client, shutdown, join, part, quit, nick;
    EventHolder check_event;
    HelpTopicHolder opbothelp, ophelp, checkhelp, matchhelp, addhelp, removehelp, edithelp;
    HelpIndexHolder index;

    opbot()
        : dno(GlobalSettingsManager::get_instance()->get("opbot:donotop")),
          old(GlobalSettingsManager::get_instance()->get("opbot:expireddonotop")),
          lostops(GlobalSettingsManager::get_instance()->get("opbot:lostops")),
          opbothelp("opbot", "opadmin", help_opbot),
          ophelp("op", "opadmin", help_op),
          checkhelp("check", "opadmin", help_check),
          matchhelp("match", "opadmin", help_match),
          addhelp("add", "opadmin", help_add),
          removehelp("remove", "opadmin", help_remove),
          edithelp("edit", "opadmin", help_edit),
          index("opbot", "opadmin")
    {
        add = add_handler(filter_command_type("add", sourceinfo::IrcCommand).requires_privilege("opadmin"),
                            &opbot::do_add);
        remove = add_handler(filter_command_type("remove", sourceinfo::IrcCommand).requires_privilege("opadmin"),
                            &opbot::do_remove);
        list = add_handler(filter_command_type("list", sourceinfo::IrcCommand).requires_privilege("opadmin"),
                            &opbot::do_list);
        check = add_handler(filter_command_type("check", sourceinfo::IrcCommand).requires_privilege("opadmin"),
                            &opbot::do_check);
        op = add_handler(filter_command_type("op", sourceinfo::IrcCommand).requires_privilege("opadmin"),
                            &opbot::do_op);
        change = add_handler(filter_command_type("edit", sourceinfo::IrcCommand).requires_privilege("opadmin"),
                            &opbot::do_change);
        match_client = add_handler(filter_command_type("match", sourceinfo::IrcCommand).requires_privilege("opadmin"),
                            &opbot::do_match);
        quit = add_handler(filter_command_type("QUIT", sourceinfo::RawIrc),&opbot::irc_depart, true, Message::first);
        part = add_handler(filter_command_type("PART", sourceinfo::RawIrc),&opbot::irc_depart, true, Message::first);
        join = add_handler(filter_command_type("JOIN", sourceinfo::RawIrc),&opbot::irc_join,true);
        nick = add_handler(filter_command_type("NICK", sourceinfo::RawIrc),&opbot::irc_nick,true);

        check_event = add_recurring_event(60, &opbot::check_expiry);

        StorageManager::get_instance()->auto_save(&dno, "donotop");
        StorageManager::get_instance()->auto_save(&old, "expireddonotop");
        StorageManager::get_instance()->auto_save(&lostops, "lostops");

        load_lists();
    }
};

MODULE_CLASS(opbot)

